#!/bin/sh
# install-subvolume.sh: ztOS next to the Linux already on a Fujitsu ARROWS Tab,
# without repartitioning. Run as root on the tablet itself:
#
#   sh install-subvolume.sh ztos-fujitsu-tablet-<ver>-x86_64.tar.zst
#
# It makes the btrfs subvolumes @ztos (the root) and @ztos-home (its /home) at
# the top level of the root filesystem's btrfs, unpacks ztOS into them, copies
# the kernel to the EFI partition's /ztos directory and adds the boot entry
# ztos.conf. The other system's subvolumes, kernels, loader entries and the
# default boot entry are not touched. Try ztOS once with
#   bootctl set-oneshot ztos.conf && reboot
# (a trial boot that does not come up goes back to the default entry by itself).
#
# Environment: ZTOS_PASSWORD_FILE  the user's password (first line), else unchanged
#              ZTOS_REPLACE=1      an @ztos already there is moved to @ztos-prev
#              ZTOS_COPY_WIFI=1    copy this system's NetworkManager profiles (default)
#              ZTOS_COPY_HOSTKEYS=1 give ztOS this system's SSH host keys (default), so
#                                  one known_hosts entry works for both; ztOS's own
#                                  are kept in /etc/ssh/ztos-own-hostkeys
set -eu
TAR=$(readlink -f "$1")
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }
[ -f "$TAR" ] || { echo "no $TAR" >&2; exit 1; }
ESP=$(findmnt -no TARGET -T /boot/loader 2>/dev/null || true)
[ -n "$ESP" ] && [ -d "$ESP/loader" ] || { echo "no systemd-boot ESP at /boot" >&2; exit 1; }
DEV=$(findmnt -no SOURCE / | sed 's/\[.*//')
[ "$(findmnt -no FSTYPE /)" = btrfs ] || { echo "/ is not btrfs" >&2; exit 1; }
ROOTUUID=$(blkid -s UUID -o value "$DEV")
ESPUUID=$(blkid -s UUID -o value "$(findmnt -no SOURCE "$ESP")")
TOP=/run/ztos-top
mkdir -p "$TOP"
mountpoint -q "$TOP" || mount -o subvolid=5 "$DEV" "$TOP"
trap 'umount "$TOP" 2>/dev/null || true' EXIT

if [ -e "$TOP/@ztos" ]; then
	[ "${ZTOS_REPLACE:-0}" = 1 ] || { echo "@ztos is already there (ZTOS_REPLACE=1 to replace it)" >&2; exit 1; }
	[ -e "$TOP/@ztos-prev" ] && btrfs subvolume delete "$TOP/@ztos-prev"
	mv "$TOP/@ztos" "$TOP/@ztos-prev"
	echo "the previous ztOS root is @ztos-prev"
fi
btrfs subvolume create "$TOP/@ztos"
NEWHOME=0
[ -e "$TOP/@ztos-home" ] || { btrfs subvolume create "$TOP/@ztos-home"; NEWHOME=1; }

echo "unpacking ztOS into @ztos"
bsdtar -xpf "$TAR" -C "$TOP/@ztos" --numeric-owner
if [ $NEWHOME = 1 ]; then
	cp -a "$TOP/@ztos/home/." "$TOP/@ztos-home/"
fi
find "$TOP/@ztos/home" -mindepth 1 -maxdepth 1 -exec rm -rf {} +

R=$TOP/@ztos
sed -i -e "s/@ROOT@/$ROOTUUID/g" -e "s/@ESP@/$ESPUUID/g" "$R/etc/fstab"
. "$R/etc/ztos/user.conf"
if [ -n "${ZTOS_PASSWORD_FILE:-}" ]; then
	printf '%s:%s\n' "$ZTOS_USER" "$(head -n1 "$ZTOS_PASSWORD_FILE")" | chpasswd -R "$R"
fi
if [ "${ZTOS_COPY_WIFI:-1}" = 1 ] && [ -d /etc/NetworkManager/system-connections ]; then
	install -d -m700 "$R/etc/NetworkManager/system-connections"
	for f in /etc/NetworkManager/system-connections/*.nmconnection; do
		[ -f "$f" ] && install -m600 -o0 -g0 "$f" "$R/etc/NetworkManager/system-connections/"
	done
fi

if [ "${ZTOS_COPY_HOSTKEYS:-1}" = 1 ] && ls /etc/ssh/ssh_host_*_key >/dev/null 2>&1; then
	install -d -m700 "$R/etc/ssh/ztos-own-hostkeys"
	for f in "$R"/etc/ssh/ssh_host_*; do
		[ -f "$f" ] && mv -f "$f" "$R/etc/ssh/ztos-own-hostkeys/"
	done
	for f in /etc/ssh/ssh_host_*; do
		case $f in *.pub) m=644 ;; *) m=600 ;; esac
		install -m$m -o0 -g0 "$f" "$R/etc/ssh/"
	done
fi

echo "the kernel to $ESP/ztos"
install -d "$ESP/ztos" "$ESP/loader/entries"
for f in vmlinuz-linux initramfs-linux.img intel-ucode.img; do
	cp "$R/boot/$f" "$ESP/ztos/.$f.new" && mv -f "$ESP/ztos/.$f.new" "$ESP/ztos/$f"
done
# sort-key after the other system's, so it stays the default when none is set
cat > "$ESP/loader/entries/.ztos.conf.new" <<ENTRY
title      ztOS
sort-key   zz-ztos
linux      /ztos/vmlinuz-linux
initrd     /ztos/intel-ucode.img
initrd     /ztos/initramfs-linux.img
options    root=UUID=$ROOTUUID rootflags=subvol=@ztos rw rootfstype=btrfs panic=10 softlockup_panic=1 rd.emergency=reboot rd.shell=0 loglevel=4
ENTRY
mv -f "$ESP/loader/entries/.ztos.conf.new" "$ESP/loader/entries/ztos.conf"
sync
echo "ztOS is installed. The default entry is unchanged:"
bootctl list --no-pager 2>/dev/null | grep -E 'title|id:' || true
echo "try it once: bootctl set-oneshot ztos.conf && reboot"
