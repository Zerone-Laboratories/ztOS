#!/usr/bin/env bash
# Apply matching product IDs and suppress unsupported ID attestation requests.
set -euo pipefail
[[ $EUID == 0 ]] || { echo 'Run as root.' >&2; exit 1; }
. /usr/lib/ztos/user.sh
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
backupdir=/var/lib/waydroid/ztos-backup/wa-link
backup=$backupdir/waydroid.cfg

mkdir -p "$backupdir"
cp -an /var/lib/waydroid/waydroid.cfg "$backup"

if [[ -S $ZTOS_RUNTIME_DIR/bus ]]; then
	ztos_as_user timeout 25 waydroid session stop || true
	ztos_as_user systemctl --user stop ztos-waydroid-session.service || true
fi
systemctl stop ztos-waydroid-network.service || true
systemctl stop waydroid-container.service
[[ $(lxc-info -P /var/lib/waydroid/lxc -n waydroid -sH 2>/dev/null || true) == STOPPED ]] || {
	echo 'Waydroid container did not stop.' >&2; exit 1;
}

python3 - "$here/waydroid.cfg" <<'PY'
import configparser
import os
import sys

target = '/var/lib/waydroid/waydroid.cfg'
current = configparser.ConfigParser()
current.read(target)
source = configparser.ConfigParser()
source.read(sys.argv[1])
if not source.has_section('properties'):
    raise SystemExit('Source config has no [properties] section.')
if not current.has_section('properties'):
    current.add_section('properties')
keys = (
    'ro.product.brand_for_attestation',
    'ro.product.device_for_attestation',
    'ro.product.manufacturer_for_attestation',
    'ro.product.model_for_attestation',
    'ro.product.name_for_attestation',
    'ro.product.product_for_attestation',
)
for key in keys:
    if key not in source['properties']:
        raise SystemExit(f'Missing {key} in source config.')
    current['properties'][key] = source['properties'][key]
temporary = target + '.ztos-new'
with open(temporary, 'w') as output:
    current.write(output)
os.chmod(temporary, 0o644)
os.replace(temporary, target)
PY

images=/usr/share/waydroid-extra/images
work=$(mktemp -d "$images/.wa-link.XXXXXX")
cleanup() {
	if mountpoint -q "$work/root"; then umount "$work/root" || return; fi
	rm -rf -- "$work"
}
trap cleanup EXIT
mkdir "$work/root"
mount -o loop,rw "$images/system.img" "$work/root"
framework="$work/root/system/framework/framework.jar"
cp -an "$framework" "$backupdir/framework.jar"
python3 "$here/patch-id-attestation.py" "$framework" "$work/framework.patched.jar"
install -m644 "$work/framework.patched.jar" "$framework"
umount "$work/root"
e2fsck -fy "$images/system.img" >/var/tmp/wa-link/system-image-fsck.log 2>&1 || {
	result=$?
	(( result <= 1 )) || { cat /var/tmp/wa-link/system-image-fsck.log >&2; exit "$result"; }
}

waydroid upgrade -o
systemctl start waydroid-container.service
if [[ -S $ZTOS_RUNTIME_DIR/bus ]]; then
	ztos_as_user systemctl --user start ztos-waydroid-session.service
fi
# Framework changes trigger ART recompilation during Android boot. This host's
# LXC attach path rejects root, so avoid Android-shell readiness and odrefresh.
systemctl start ztos-waydroid-network.service
echo 'Applied attestation compatibility patch; Android is booting and network policy is active.'
