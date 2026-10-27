#!/usr/bin/env bash
# Restore Waydroid's original framework.jar and config without touching app data.
set -euo pipefail
[[ $EUID == 0 ]] || { echo 'Run as root.' >&2; exit 1; }
. /usr/lib/ztos/user.sh
backupdir=/var/lib/waydroid/ztos-backup/wa-link
images=/usr/share/waydroid-extra/images
[[ -s $backupdir/framework.jar && -s $backupdir/waydroid.cfg ]] || {
	echo 'Attestation rollback backup is incomplete.' >&2; exit 1;
}

if [[ -S $ZTOS_RUNTIME_DIR/bus ]]; then
	ztos_as_user timeout 25 waydroid session stop || true
	ztos_as_user systemctl --user stop ztos-waydroid-session.service || true
fi
systemctl stop waydroid-container.service
[[ $(lxc-info -P /var/lib/waydroid/lxc -n waydroid -sH 2>/dev/null || true) == STOPPED ]] || {
	echo 'Waydroid container did not stop.' >&2; exit 1;
}

work=$(mktemp -d "$images/.wa-link-rollback.XXXXXX")
cleanup() {
	if mountpoint -q "$work/root"; then umount "$work/root" || return; fi
	rm -rf -- "$work"
}
trap cleanup EXIT
mkdir "$work/root"
mount -o loop,rw "$images/system.img" "$work/root"
install -m644 "$backupdir/framework.jar" \
	"$work/root/system/framework/framework.jar"
umount "$work/root"
e2fsck -fy "$images/system.img" >/var/tmp/wa-link/rollback-fsck.log 2>&1 || {
	result=$?
	(( result <= 1 )) || { cat /var/tmp/wa-link/rollback-fsck.log >&2; exit "$result"; }
}
install -m644 "$backupdir/waydroid.cfg" /var/lib/waydroid/waydroid.cfg
waydroid upgrade -o
systemctl start waydroid-container.service
systemctl restart ztos-waydroid-network.service
if [[ -S $ZTOS_RUNTIME_DIR/bus ]]; then
	ztos_as_user systemctl --user start ztos-waydroid-session.service
fi
timeout 90 /usr/local/libexec/ztos-waydroid-recover ready
echo 'Restored original framework.jar and Waydroid config.'
