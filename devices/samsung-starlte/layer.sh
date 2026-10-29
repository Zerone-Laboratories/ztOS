# The Galaxy S9 (starlte) device layer, run by the installer inside the build
# namespace after the GSI part, with $R (the root), $DEVSRC (this folder:
# vendor/, overlay/, packages.txt), $DEVROOT (the device repository: kernel/,
# firmware/, prebuilts/...; ztOSMobile itself before the split), $P (pacman),
# step and ch (run in the root). Everything here is the S9's own: the GSI
# carries none of it.
D=$DEVSRC

step "adding the Galaxy S9 device layer"
# (packages.txt went in with the GSI's packages, and the kernel was held
# back from pacman before the first update: build.py)

# ztOSIAL: the manifest and the S9's backends
install -d "$R/vendor/ztos"
cp -a "$D/vendor/ztos/." "$R/vendor/ztos/"
chown -R 0:0 "$R/vendor"

cp -a "$D/overlay/." "$R/"                   # the phone's own: charger, panel, sensor hub, modem, sleep
if [ -f "$R/etc/fstab" ]; then
	sed -i 's/\([[:space:]]\)default\([,[:space:]]\)/\1defaults\2/g' "$R/etc/fstab"
fi
# the panel's brightness table is made from its calibration (ztos-panel-bl)
install -Dm755 "$DEVROOT/kernel/s9p-bl-table.py" "$R/usr/local/lib/ztos/s9p-bl-table.py"
# SYS_SLEEP needs its backend and RTC/TMU helpers as well as the daemon.
PW=$R/usr/local/libexec/ztos-power
install -d "$PW/mif-trial"
install -m755 "$DEVROOT/third_party/s9plus/extras/debug-tools/files/usr/local/sbin/s9p-acpm" "$PW/s9p-acpm"
install -m755 "$DEVROOT/power/rtc/initialize-calendar.py" \
	"$DEVROOT/power/suspend/load-acpm-tmu.py" "$PW/mif-trial/"
install -m644 "$DEVROOT/prebuilts/power/zrn-sleep-init.ko" \
	"$DEVROOT/prebuilts/power/zrn-exynos-rtc.ko" \
	"$DEVROOT/prebuilts/power/exynos9810_acpm_tmu.fw" "$PW/mif-trial/"
# the Windows Phone / Lumia photos, made from the OneDrive zip by os/wallpapers/make-wallpapers.py
if [ -d "$DEVROOT/build/wallpapers/backgrounds/ztos" ]; then
	install -d "$R/usr/local/share/backgrounds"
	cp -a "$DEVROOT/build/wallpapers/backgrounds/ztos" "$R/usr/local/share/backgrounds/"
fi
install -Dm644 -t "$R/usr/lib/firmware/brcm" "$DEVROOT"/firmware/bcmdhd* "$DEVROOT"/firmware/nvram*
install -Dm644 "$DEVROOT/third_party/s9plus/firmware/bcm4361B2_semco.hcd" \
	"$R/usr/lib/firmware/brcm/BCM.samsung,starlte.hcd"
for f in "$DEVROOT"/firmware/calliope_*.bin; do
	[ -f "$f" ] && install -Dm644 -t "$R/usr/lib/firmware" "$f"
done
install -d "$R/usr/lib/modules/7.1.0-zboot/extra"
install -d "$R/usr/local/lib"
if [ -f "$DEVROOT/modem/zrn-modem/zrn-modem.ko" ]; then
	install -m644 "$DEVROOT/modem/zrn-modem/zrn-modem.ko" "$R/usr/local/lib/zrn-modem.ko"
	install -m644 "$DEVROOT/modem/zrn-modem/zrn-modem.ko" "$R/usr/lib/modules/7.1.0-zboot/extra/"
elif [ -f "$DEVROOT/prebuilts/modules/zrn-modem.ko" ]; then
	install -m644 "$DEVROOT/prebuilts/modules/zrn-modem.ko" "$R/usr/local/lib/zrn-modem.ko"
	install -m644 "$DEVROOT/prebuilts/modules/zrn-modem.ko" "$R/usr/lib/modules/7.1.0-zboot/extra/"
fi
MODULES="$DEVROOT/prebuilts/modules/*.ko $DEVROOT/power/idle/zrn-cpuidle.ko $DEVROOT/haptics/zrn-haptic.ko
	$DEVROOT/power/hotplug/zrn-hotplug.ko $DEVROOT/leds/zrn-led.ko $DEVROOT/sensors/driver/zrn-bbd.ko
	$DEVROOT/audio/driver/snd-zrn-abox.ko $DEVROOT/audio/modules/*.ko $DEVROOT/score/driver/zrn-score.ko"
for k in $MODULES; do
	[ -f "$k" ] && install -m644 "$k" "$R/usr/lib/modules/7.1.0-zboot/extra/"
done
if [ -f "$DEVROOT/build/linux-7.1/out/modules.builtin" ]; then
	install -m644 "$DEVROOT/build/linux-7.1/out/modules.builtin" "$R/usr/lib/modules/7.1.0-zboot/"
	install -m644 "$DEVROOT/build/linux-7.1/out/modules.builtin.modinfo" "$R/usr/lib/modules/7.1.0-zboot/"
fi
ch depmod -a 7.1.0-zboot > /dev/null 2>&1 || true
for moddir in "$R"/usr/lib/modules/*; do
	[ -d "$moddir" ] || continue
	kver=$(basename "$moddir")
	[ "$kver" = "7.1.0-zboot" ] && continue
	install -d "$moddir/extra"
	for k in $MODULES; do
		[ -f "$k" ] && install -m644 "$k" "$moddir/extra/"
	done
	ch depmod -a "$kver" > /dev/null 2>&1 || true
done
chmod 755 "$R"/usr/local/sbin/* 2>/dev/null || true

DEVICE_UNITS="zboot-boot-ok zboot-usb-gadget zboot-firstboot zboot-charge-hold zrn-charge zrn-cpuidle ztoscirrusaudio
	ztoscsf-sensorhub ztoscsf-sensord ztoscorenetworking-modem ztoscorenetworking zrnperfd zrn-sleepd
	ztos-panel-selfmask ztos-panel-bl zrn-bluetooth bluetooth ztos-gnss-gate ztos-gnss-lto.timer ztos-waydroid-location"
