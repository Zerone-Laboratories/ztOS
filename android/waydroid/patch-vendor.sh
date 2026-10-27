#!/usr/bin/env bash
# Work on an unmounted copy; preserve vendor.img.stock for rollback.
set -euo pipefail
[[ $EUID == 0 ]] || exit 1
here=$(cd -- "$(dirname -- "$0")" && pwd)
images=/usr/share/waydroid-extra/images
[[ $(lxc-info -P /var/lib/waydroid/lxc -n waydroid -sH 2>/dev/null || true) == STOPPED ]] || {
    echo 'Stop the Waydroid session and container service first.' >&2; exit 1;
}
python3 - <<'PYCONFIG'
import configparser
config = configparser.ConfigParser()
config.read('/var/lib/waydroid/waydroid.cfg')
if config.get('properties', 'persist.waydroid.width', fallback='') != '360' or config.get('properties', 'persist.waydroid.height', fallback='') != '672':
    raise SystemExit('Composer hotplug fix requires fixed 360x672 logical geometry.')
PYCONFIG
cp -an --reflink=auto --sparse=always "$images/vendor.img" "$images/vendor.img.stock"
work=$(mktemp -d "$images/.vendor.XXXXXX")
cleanup() {
    if mountpoint -q "$work/root"; then umount "$work/root" || return; fi
    rm -rf -- "$work"
}
trap cleanup EXIT
cp --reflink=auto --sparse=always --preserve=all "$images/vendor.img.stock" "$work/vendor.img"
mkdir "$work/root"
mount -o loop,rw "$work/vendor.img" "$work/root"
python3 "$here/patch-composer.py" "$work/root/lib64/hw/hwcomposer.waydroid.so"
umount "$work/root"
rc=0
e2fsck -fy "$work/vendor.img" || rc=$?
(( rc <= 1 ))
mv -T "$work/vendor.img" "$images/vendor.img"
