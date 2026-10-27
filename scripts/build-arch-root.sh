#!/bin/bash
# Arch Linux ARM tarball -> ext4 image labelled ZR_ARCH, for USERDATA.
#
#   scripts/build-arch-root.sh [size, default 6G]
#
# Runs unprivileged: the tarball is unpacked and the image is populated under
# fakeroot (mke2fs -d), so ownership and modes are right without a loop mount.
# What fakeroot cannot carry are extended attributes; the file capabilities of
# a few binaries (newuidmap, newgidmap) come back with their next package
# update. No chroot is used: everything that has to *run* on the target
# (pacman-key, growing the filesystem) is left to a first-boot unit.
#
# Output: out/arch-root.img (sparse). It grows to the whole partition on the
# first boot.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
B=$ROOT/build
TAR=$B/alarm.tar.gz
R=$B/arch-root
STATE=$B/arch-root.fakeroot
SIZE=${1:-6G}
OVERLAY=$ROOT/rootfs/overlay
KEY=${ZBOOT_SSH_KEY:-$HOME/.ssh/id_ed25519.pub}

[ -s "$TAR" ] || { echo "missing $TAR" >&2; exit 1; }
if [ -s "$TAR.md5" ]; then
	[ "$(md5sum "$TAR" | cut -d' ' -f1)" = "$(cut -d' ' -f1 "$TAR.md5")" ] ||
		{ echo "md5 mismatch on $TAR" >&2; exit 1; }
	echo "tarball md5 ok"
fi

chmod -R u+rwX "$R" 2>/dev/null || true
rm -rf "$R" "$STATE"
mkdir -p "$R" "$ROOT/out"

echo "=== unpack ==="
fakeroot -s "$STATE" -- bsdtar -xpf "$TAR" -C "$R" --no-xattrs --no-acls --no-fflags

echo "=== configure ==="
fakeroot -i "$STATE" -s "$STATE" -- bash -euo pipefail -s "$R" "$OVERLAY" "$KEY" "$ROOT" <<'EOS'
R=$1 OVERLAY=$2 KEY=$3 ROOT=$4
# the user helpers (user.sh, ztos_user.py, ztos-user-dropins) are ztOS's, not
# the phone overlay's; the overlay's own templates go over them
install -d "$R/usr/lib/ztos"
cp -a --no-preserve=ownership "$ROOT/os/overlay/usr/lib/ztos/." "$R/usr/lib/ztos/"
install -Dm755 "$ROOT/os/overlay/usr/local/sbin/ztos-user-dropins" "$R/usr/local/sbin/ztos-user-dropins"
cp -a --no-preserve=ownership "$OVERLAY/." "$R/"
chown -R 0:0 "$R/etc/systemd" "$R/usr/local/sbin" "$R/etc/fstab" "$R/etc/hostname"
chmod 755 "$R"/usr/local/sbin/zboot-*
# sudo ignores a sudoers.d file that is not 0440 root:root
chown 0:0 "$R/etc/sudoers.d/10-wheel"
chmod 440 "$R/etc/sudoers.d/10-wheel"

# the kernel lives in the boot image, never in a package
sed -i 's/^#\?IgnorePkg *=.*/IgnorePkg   = linux-aarch64 linux-aarch64-headers linux-firmware/' "$R/etc/pacman.conf"
grep -q '^IgnorePkg' "$R/etc/pacman.conf"

W=$R/etc/systemd/system/multi-user.target.wants
mkdir -p "$W" "$R/etc/systemd/system/getty.target.wants"
ln -sf /etc/systemd/system/zboot-usb-gadget.service "$W/zboot-usb-gadget.service"
ln -sf /etc/systemd/system/zboot-firstboot.service "$W/zboot-firstboot.service"
ln -sf /usr/lib/systemd/system/serial-getty@.service "$R/etc/systemd/system/getty.target.wants/serial-getty@ttyGS0.service"
# no suspend: the big cores do not come back from it on this kernel
for t in sleep suspend hibernate hybrid-sleep suspend-then-hibernate; do
	ln -sf /dev/null "$R/etc/systemd/system/$t.target"
done
touch "$R/etc/zboot-firstboot"

# the stock image's user, alarm, is the phone's (zboot-firstboot fills in
# the unit drop-ins, greetd and sudoers from this)
IFS=: read -r _ _ uid gid _ home _ < <(grep '^alarm:' "$R/etc/passwd")
install -Dm644 /dev/stdin "$R/etc/ztos/user.conf" <<USERCONF
ZTOS_USER=alarm
ZTOS_UID=$uid
ZTOS_GID=$gid
ZTOS_HOME=$home
USERCONF

if [ -s "$KEY" ]; then
	for home in "$R/root" "$R/home/alarm"; do
		install -d -m 700 "$home/.ssh"
		install -m 600 "$KEY" "$home/.ssh/authorized_keys"
	done
	chown -R "$uid:$gid" "$R/home/alarm/.ssh"
	chown -R 0:0 "$R/root/.ssh"
fi
EOS

echo "=== image ==="
rm -f "$ROOT/out/arch-root.img"
fakeroot -i "$STATE" -- /usr/bin/mke2fs -q -t ext4 -L ZR_ARCH -m 1 -O ^orphan_file -d "$R" "$ROOT/out/arch-root.img" "$SIZE"
/usr/bin/e2fsck -fn "$ROOT/out/arch-root.img" | tail -n 2
ls -ls "$ROOT/out/arch-root.img"
echo "ARCH_ROOT_OK $ROOT/out/arch-root.img"
