#!/bin/sh
# Step 8.1: patch init's CAP_WAKE_ALARM drop in a work copy of the live
# system.img, check it, and move it over system.img. The running container
# keeps the old inode until the reboot. Any failure: the work copy goes,
# system.img and the backup stay as they are.
set -u
d=/home/zerone/team-r2-holes
cd /usr/share/waydroid-extra/images || exit 1
m=$(mktemp -d /run/r2-init.XXXXXX)
fail() {
    echo "FAIL: $*"
    mountpoint -q "$m" && umount "$m"
    rm -f .system.img.r2
    rmdir "$m"
    exit 1
}
df -h /usr/share/waydroid-extra/images | tail -1
if [ ! -e system.img.before-r2 ]; then
    cp --sparse=always --preserve=all system.img system.img.before-r2 || { rm -f system.img.before-r2; fail backup; }
fi
cmp -s system.img system.img.before-r2 || fail "backup differs from system.img"
cp --sparse=always --preserve=all system.img .system.img.r2 || fail copy
mount -o loop,rw .system.img.r2 "$m" || fail "mount rw"
sha256sum "$m/system/bin/init"
stat -c '%a %U:%G' "$m/system/bin/init"
getfattr --only-values -n security.selinux "$m/system/bin/init" 2>/dev/null | tr -d '\000'; echo
python3 "$d/patch-init-wake-alarm.py" "$m/system/bin/init" || fail "patch"
stat -c '%a %U:%G' "$m/system/bin/init"
getfattr --only-values -n security.selinux "$m/system/bin/init" 2>/dev/null | tr -d '\000'; echo
sync
umount "$m" || fail umount
rc=0
e2fsck -fy .system.img.r2 > "$d/swap-e2fsck.out" 2>&1 || rc=$?
tail -3 "$d/swap-e2fsck.out"
[ "$rc" -le 1 ] || fail "e2fsck rc $rc"
mount -o loop,ro .system.img.r2 "$m" || fail "mount ro"
h=$(sha256sum "$m/system/bin/init" | cut -d' ' -f1)
umount "$m" || fail "umount ro"
[ "$h" = d01768fd0cf0b5a18557a9d721f3b6b2df42392905a892abf84c8c77debf1e08 ] || fail "init hash $h"
mv -T .system.img.r2 system.img || fail mv
rmdir "$m"
sync
ls -la /usr/share/waydroid-extra/images
echo "SWAP OK"
