# The QEMU device layer, run by the installer inside the build namespace with
# $R (the root), $DEVSRC (this folder), $STAGEDIR, and ch (run in the root).
# Everything the emulator needs that the GSI does not have.
install -d "$R/vendor/ztos"
cp -a "$DEVSRC/vendor/ztos/." "$R/vendor/ztos/"
[ -d "$DEVROOT/os/overlay-qemu" ] && cp -a "$DEVROOT/os/overlay-qemu/." "$R/"
cat > "$R/etc/fstab" <<'FSTAB'
# ztOS in QEMU
PARTLABEL=ztos-root  /          ext4  rw,relatime  0 1
PARTLABEL=ztos-esp   /boot/efi  vfat  rw,relatime,fmask=0077,dmask=0077  0 2
FSTAB
mkdir -p "$R/boot/efi"
step "making the start-up image (initramfs)"
ch mkinitcpio -P > /dev/null 2>&1 || ch mkinitcpio -P
# what the boot partition takes, readable outside this namespace
install -d "$STAGEDIR/boot"
install -m644 "$R/boot/Image" "$STAGEDIR/boot/Image"
install -m644 "$R/boot/initramfs-linux.img" "$STAGEDIR/boot/initramfs.img"
install -m644 "$R/usr/lib/systemd/boot/efi/systemd-bootaa64.efi" "$STAGEDIR/boot/BOOTAA64.EFI"
