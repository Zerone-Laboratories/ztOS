# The Fujitsu ARROWS Tab device layer (x86_64), run by the installer inside
# the build namespace after the GSI part, with $R (the root), $DEVSRC (this
# folder), $DEVROOT (the repository), $STAGEDIR, step and ch (run in the root).
#
# ztOS lives here next to another Linux on the same disk: its root is the
# btrfs subvolume @ztos and /home is @ztos-home (install-subvolume.sh makes
# them). The EFI system partition is the other system's /boot; ztOS mounts it
# at /efi and only ever writes its own directory there, /efi/ztos (kernel,
# initramfs, microcode), through ztos-esp-sync. Its own /boot stays in @ztos.
D=$DEVSRC

step "adding the Fujitsu ARROWS Tab device layer"
install -d "$R/vendor/ztos"
cp -a "$D/vendor/ztos/." "$R/vendor/ztos/"
cp -a "$D/overlay/." "$R/"
chown -R 0:0 "$R/vendor" "$R/etc/skel/.config"
chmod 755 "$R"/usr/local/sbin/ztos-wakeups "$R"/usr/local/sbin/ztos-esp-sync "$R"/usr/local/sbin/ztos-bootwatch "$R"/usr/local/sbin/ztos-emergency

# the tablet's shell pieces (ztmetro's install-desktop has them; make install not)
install -Dm755 "$SRC/ztmetro/desktop/zrn-iio-orient" "$R/usr/local/bin/zrn-iio-orient"
install -Dm644 "$SRC/ztmetro/desktop/50-zrn-iio-orient.rules" "$R/etc/polkit-1/rules.d/50-zrn-iio-orient.rules"

# zrn-touchguard: this tablet's touch panel fires ghost touches (stuck points,
# lines that blink); the guard grabs it and passes on only real contacts
rm -rf "$R/usr/src/zrn-touchguard"; mkdir -p "$R/usr/src"
cp -a "$DEVROOT/input/touchguard" "$R/usr/src/zrn-touchguard"
ch sh -c 'cd /usr/src/zrn-touchguard && make clean >/dev/null 2>&1; make && make install PREFIX=/usr/local'
rm -rf "$R/usr/src/zrn-touchguard"

# ztOSCOR-U starts the S9's hardware daemons; a PC has none of them
cat > "$R/etc/ztoscoru/components" <<'COMP'
# ztOSCOR-U on the Fujitsu ARROWS Tab: no phone hardware daemons (sensors come
# from iio-sensor-proxy, sound from PipeWire on HD Audio, no modem).
# kind|id|argument|title
session|ztapps||App list
COMP

# (install-subvolume.sh puts this tablet's filesystem UUIDs in for @ROOT@ and @ESP@)
cat > "$R/etc/fstab" <<'FSTAB'
# ztOS on the Fujitsu ARROWS Tab: btrfs subvolumes next to the other system
UUID=@ROOT@  /      btrfs  rw,relatime,compress=zstd:3,ssd,discard=async,subvol=/@ztos       0 0
UUID=@ROOT@  /home  btrfs  rw,relatime,compress=zstd:3,ssd,discard=async,subvol=/@ztos-home  0 0
# the EFI system partition (the other system's /boot): ztOS writes only /efi/ztos
UUID=@ESP@   /efi   vfat   noauto,x-systemd.automount,x-systemd.idle-timeout=60,rw,fmask=0077,dmask=0077  0 2
FSTAB
install -d "$R/efi"

# mDNS: the tablet answers as <hostname>.local
sed -i 's/^hosts: .*/hosts: mymachines mdns_minimal [NOTFOUND=return] resolve [!UNAVAIL=return] files myhostname dns/' "$R/etc/nsswitch.conf"

step "making the start-up image (initramfs)"
# no autodetect: built here, it would keep only this computer's drivers
ch mkinitcpio -P > /dev/null 2>&1 || ch mkinitcpio -P

DEVICE_UNITS="ztos-wakeups zrn-touchguard bluetooth avahi-daemon ztos-bootwatch.timer systemd-timesyncd"
# the S9's clocks and cores: nothing for zrnperfd to do on a PC
DEVICE_OFF="zrnperfd"
