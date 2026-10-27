#!/usr/bin/env bash
# Install a prepared, checksum-verified microG payload without changing Android data.
# Usage on phone: sudo bash install-microg.sh PAYLOAD_TAR
# Roll back: sudo bash install-microg.sh --rollback
set -euo pipefail
[[ $EUID == 0 ]] || { echo 'Run as root.' >&2; exit 1; }
. /usr/lib/ztos/user.sh
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
image=/usr/share/waydroid-extra/images/system.img
backup=/var/lib/waydroid/ztos-backup/microg
work=/var/tmp/wa-link/microg
mkdir -p "$backup" "$work/root"
userctl() {
    ztos_as_user "$@"
}
restart() {
    systemctl start waydroid-container.service
    userctl systemctl --user start ztos-waydroid-session.service
    systemctl start ztos-waydroid-network.service
}
if [[ ${1:-} != --rollback ]]; then
    [[ -s ${1:?Prepared payload tar required} ]]
    # Validate patch compatibility before stopping Android or changing its image.
    python3 "$here/patch-microg-spoofing.py" \
        /var/lib/waydroid/rootfs/system/framework/services.jar "$work/services.jar"
fi
userctl systemctl --user stop ztos-waydroid-session.service
userctl timeout 25 waydroid session stop || true
systemctl stop ztos-waydroid-network.service
systemctl stop waydroid-container.service
if pgrep -f '^lxc-start .* -n waydroid ' >/dev/null; then
    echo 'Container still running; refusing image edit.' >&2
    exit 1
fi
if [[ ${1:-} == --rollback ]]; then
    [[ -s $backup/system.img ]]
    cp --reflink=auto "$backup/system.img" "$image"
    restart
    echo 'Restored pre-microG system image; Android data retained.'
    exit
fi
cp --reflink=auto -an "$image" "$backup/system.img"
cp -an /var/lib/waydroid/waydroid.cfg "$backup/waydroid.cfg"
cleanup() {
    if mountpoint -q "$work/root"; then umount "$work/root"; fi
}
trap cleanup EXIT
# Grow only when the image cannot hold the 144 MiB arm64 payload plus headroom.
e2fsck -fy "$image" >"$work/fsck.log" 2>&1 || { status=$?; (( status <= 1 )); }
free=$(dumpe2fs -h "$image" 2>/dev/null | awk '/Free blocks:/ {print $3}')
block=$(dumpe2fs -h "$image" 2>/dev/null | awk '/Block size:/ {print $3}')
if (( free * block < 256 * 1024 * 1024 )); then
    truncate -s +512M "$image"
    resize2fs "$image" >"$work/resize.log" 2>&1
fi
mount -o loop,rw "$image" "$work/root"
tar -xf "$1" -C "$work/root"
install -m644 "$work/services.jar" "$work/root/system/framework/services.jar"
for path in MicroGGmsCore MicroGGsfProxy MicroGFakeStore; do
    chown -R 0:0 "$work/root/system/priv-app/$path"
    find "$work/root/system/priv-app/$path" -type d -exec chmod 755 {} +
    find "$work/root/system/priv-app/$path" -type f -exec chmod 644 {} +
done
chown 0:0 "$work/root/system/etc/permissions/"*.microg.xml \
    "$work/root/system/etc/sysconfig/ztos-microg.xml"
chmod 644 "$work/root/system/etc/permissions/"*.microg.xml \
    "$work/root/system/etc/sysconfig/ztos-microg.xml"
sync
umount "$work/root"
e2fsck -fy "$image" >>"$work/fsck.log" 2>&1 || { status=$?; (( status <= 1 )); }
restart
echo 'microG installed; Android is booting. Verify Self-Check before writing RETRY.'
