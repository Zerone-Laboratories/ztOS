#!/usr/bin/env bash
# Apply a checked boot-library payload; retain microG, build identity, and app data.
set -euo pipefail
[[ $EUID == 0 ]] || { echo 'Run as root.' >&2; exit 1; }
. /usr/lib/ztos/user.sh
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
backup=/var/lib/waydroid/ztos-backup/oldsdk
work=/var/tmp/wa-link/oldsdk
image=/usr/share/waydroid-extra/images/system.img
mkdir -p "$backup" "$work/root" "$work/payload"
rollback=0
if [[ ${1:-} == --rollback ]]; then
    rollback=1
    [[ -s $backup/oldsdk-manifest.json ]]
    cp "$backup/oldsdk-manifest.json" "$work/payload/oldsdk-manifest.json"
else
    [[ -s ${1:?Usage: apply-old-sdk.sh PAYLOAD_TAR or --rollback} ]]
    python3 - "$1" "$work/payload" <<'PY'
import hashlib, json, pathlib, sys, tarfile
with tarfile.open(sys.argv[1]) as archive:
    manifest = json.load(archive.extractfile('oldsdk-manifest.json'))
    if manifest['version'] != 1:
        raise SystemExit('Unsupported payload version')
    for entry in manifest['jars']:
        path = pathlib.Path(entry['path'])
        if path.is_absolute() or '..' in path.parts or not str(path).startswith('system/'):
            raise SystemExit('Unsafe image path')
        data = archive.extractfile(str(path)).read()
        if hashlib.sha256(data).hexdigest() != entry['patched_sha256']:
            raise SystemExit('Payload checksum mismatch')
        target = pathlib.Path(sys.argv[2]) / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    (pathlib.Path(sys.argv[2])/'oldsdk-manifest.json').write_text(json.dumps(manifest, indent=2))
PY
fi
# Reject stale snapshots before stopping the running container.
python3 - "$work/payload/oldsdk-manifest.json" "$rollback" <<'PY'
import hashlib, json, pathlib, sys
key = 'patched_sha256' if sys.argv[2] == '1' else 'sha256'
for item in json.load(open(sys.argv[1]))['jars']:
    path = pathlib.Path('/var/lib/waydroid/rootfs')/item['path']
    if hashlib.sha256(path.read_bytes()).hexdigest() != item[key]:
        raise SystemExit(f'Live JAR differs from the checked snapshot: {path}')
PY
sha256sum /var/lib/waydroid/rootfs/system/framework/services.jar > "$work/services.before.sha256"
cp -an /var/lib/waydroid/waydroid.cfg "$backup/waydroid.cfg.before"
userctl() {
    ztos_as_user "$@"
}
userctl systemctl --user stop ztos-waydroid-session.service
userctl timeout 25 waydroid session stop || true
systemctl stop ztos-waydroid-network.service
systemctl stop waydroid-container.service
if pgrep -f '^lxc-start .* -n waydroid ' >/dev/null; then
    echo 'Container still running; refusing image edit.' >&2
    exit 1
fi
cleanup() {
    if mountpoint -q "$work/root"; then umount "$work/root"; fi
}
trap cleanup EXIT
e2fsck -fy "$image" > "$work/fsck.log" 2>&1 || { status=$?; (( status <= 1 )); }
mount -o loop,rw "$image" "$work/root"
python3 - "$work" "$backup" "$rollback" <<'PY'
import hashlib, json, os, pathlib, shutil, sys
work, backup = map(pathlib.Path, sys.argv[1:3])
rollback = sys.argv[3] == '1'
manifest = json.load(open(work/'payload/oldsdk-manifest.json'))
for item in manifest['jars']:
    target = work/'root'/item['path']
    expected = item['patched_sha256'] if rollback else item['sha256']
    if hashlib.sha256(target.read_bytes()).hexdigest() != expected:
        raise SystemExit(f'Image JAR changed: {target}')
    saved = backup/item['path']
    if not rollback:
        saved.parent.mkdir(parents=True, exist_ok=True)
        if not saved.exists():
            shutil.copy2(target, saved)
        if hashlib.sha256(saved.read_bytes()).hexdigest() != item['sha256']:
            raise SystemExit(f'Unexpected existing backup: {saved}')
    source = saved if rollback else work/'payload'/item['path']
    temporary = target.with_name(target.name + '.ztos-new')
    shutil.copyfile(source, temporary)
    os.chmod(temporary, 0o644)
    os.chown(temporary, 0, 0)
    os.replace(temporary, target)
if not rollback:
    shutil.copyfile(work/'payload/oldsdk-manifest.json', backup/'oldsdk-manifest.json')
if rollback and (backup/'boot/manifest.json').exists():
    boot = json.load(open(backup/'boot/manifest.json'))
    for name, expected in boot['installed'].items():
        target = work/'root'/name
        if hashlib.sha256(target.read_bytes()).hexdigest() != expected:
            raise SystemExit(f'Boot image changed: {target}')
    for name in boot['installed']:
        (work/'root'/name).unlink()
    for name in boot['originals']:
        shutil.copy2(backup/'boot'/name, work/'root'/name, follow_symlinks=False)
PY
expected_services=$(cut -d ' ' -f1 "$work/services.before.sha256")
actual_services=$(sha256sum "$work/root/system/framework/services.jar" | cut -d ' ' -f1)
[[ $actual_services == "$expected_services" ]] || { echo 'services.jar changed unexpectedly.' >&2; exit 1; }
sync
umount "$work/root"
e2fsck -fy "$image" >> "$work/fsck.log" 2>&1 || { status=$?; (( status <= 1 )); }
python3 - "$rollback" "$backup/waydroid.cfg.before" <<'PY'
import configparser, os, sys
path = '/var/lib/waydroid/waydroid.cfg'
config = configparser.ConfigParser()
config.read(path)
if not config.has_section('properties'):
    config.add_section('properties')
if sys.argv[1] == '1':
    old = configparser.ConfigParser()
    old.read(sys.argv[2])
    previous = old.get('properties', 'persist.ztos.oldsdk', fallback=None)
    if previous is None:
        config.remove_option('properties', 'persist.ztos.oldsdk')
    else:
        config['properties']['persist.ztos.oldsdk'] = previous
else:
    config['properties']['persist.ztos.oldsdk'] = 'com.whatsapp'
with open(path + '.ztos-new', 'w') as out:
    config.write(out)
os.chmod(path + '.ztos-new', 0o644)
os.replace(path + '.ztos-new', path)
PY
waydroid upgrade -o
systemctl start waydroid-container.service
userctl systemctl --user start ztos-waydroid-session.service
systemctl start ztos-waydroid-network.service
echo 'Old-SDK boot libraries deployed; Android is booting. App data retained.'
