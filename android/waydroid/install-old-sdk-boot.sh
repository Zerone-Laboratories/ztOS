#!/usr/bin/env bash
# Install the boot extension generated against the live patched framework.
set -euo pipefail
[[ $EUID == 0 ]] || exit 1
. /usr/lib/ztos/user.sh
source=$ZTOS_HOME/.local/share/waydroid/data/local/tmp/ztos-oldsdk-boot
backup=/var/lib/waydroid/ztos-backup/oldsdk/boot
work=/var/tmp/wa-link/oldsdk
image=/usr/share/waydroid-extra/images/system.img
[[ $(cat "$source/status") == 0 ]]
for suffix in art oat vdex; do [[ -s $source/boot-framework.$suffix ]]; done
python3 - "$source/framework-jars.sha256" <<'PY'
import hashlib, pathlib, sys
for line in pathlib.Path(sys.argv[1]).read_text().splitlines():
    expected, name = line.split()
    target = pathlib.Path('/var/lib/waydroid/rootfs')/name.lstrip('/')
    if hashlib.sha256(target.read_bytes()).hexdigest() != expected:
        raise SystemExit(f'Framework changed after boot compilation: {name}')
PY
[[ ! -e $backup/manifest.json ]] || { echo 'Boot backup already exists; inspect before repeating.' >&2; exit 1; }
mkdir -p "$backup" "$work/root"
userctl() {
    ztos_as_user "$@"
}
userctl systemctl --user stop ztos-waydroid-session.service
userctl timeout 25 waydroid session stop || true
systemctl stop ztos-waydroid-network.service waydroid-container.service
if pgrep -f '^lxc-start .* -n waydroid ' >/dev/null; then exit 1; fi
cleanup() { if mountpoint -q "$work/root"; then umount "$work/root"; fi; }
trap cleanup EXIT
e2fsck -fy "$image" > "$work/boot-fsck.log" 2>&1 || { status=$?; (( status <= 1 )); }
mount -o loop,rw "$image" "$work/root"
python3 - "$work/root" "$source" "$backup" <<'PY'
import hashlib, json, pathlib, shutil, sys
root, source, backup = map(pathlib.Path, sys.argv[1:])
names = ['framework', 'framework-graphics', 'ext', 'telephony-common',
         'voip-common', 'ims-common', 'core-icu4j']
paths = []
for name in names:
    paths += [f'system/framework/arm64/boot-{name}.{ext}' for ext in ('art', 'oat', 'vdex')]
    paths += [f'system/framework/boot-{name}.vdex']
manifest = {'originals': [], 'installed': {}}
for name in paths:
    target = root/name
    if target.exists() or target.is_symlink():
        saved = backup/name
        saved.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(target, saved, follow_symlinks=False)
        manifest['originals'].append(name)
for name in paths:
    target = root/name
    if target.exists() or target.is_symlink():
        target.unlink()
for ext in ('art', 'oat', 'vdex'):
    name = f'system/framework/arm64/boot-framework.{ext}'
    target = root/name
    shutil.copyfile(source/f'boot-framework.{ext}', target)
    target.chmod(0o644)
    manifest['installed'][name] = hashlib.sha256(target.read_bytes()).hexdigest()
(backup/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
PY
sync
umount "$work/root"
e2fsck -fy "$image" >> "$work/boot-fsck.log" 2>&1 || { status=$?; (( status <= 1 )); }
systemctl start waydroid-container.service
userctl systemctl --user start ztos-waydroid-session.service
systemctl start ztos-waydroid-network.service
echo 'Matching framework boot extension installed; Android is restarting.'
