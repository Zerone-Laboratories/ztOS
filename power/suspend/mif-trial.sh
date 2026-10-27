#!/bin/sh
# One RTC-timed SYS_SLEEP trial on the Galaxy S9, as root, from a fresh boot.
#
#   mif-trial.sh ap    AP sleep only (memory stays up): wakes by itself
#   mif-trial.sh mif   full sleep, memory interface (MIF) down as well
#   mif-trial.sh mif-cp  the same with the modem left running, as stock
#                        sleeps (exynos_check_cp_status): MIF goes down only
#                        if the modem lets go of memory by itself
#
# Puts together what the 2026-10-16 trials did by hand from /tmp: the RTC
# driver and calendar, the suspend-aware hotplug driver, the guarded backend
# with Samsung's low-power init, C2 on all cores, then the latest trial
# configuration: modem and audio off, ACPM FVP release, MIF 421 MHz and
# INT 107 MHz, USB gadget detached for the sleep.
#
# A mif trial has always hung so far. Run it only with someone at the phone:
# after a hang, force it straight into TWRP (Vol Up + Bixby + Power) so the
# console log of the hang survives, and run power/watch-recovery.py on the
# laptop to collect it. zrn-lastlog keeps a copy that survives TWRP: see
# below.
set -eu
. /usr/lib/ztos/user.sh
MODE=${1:?ap or mif}
case "$MODE" in ap) TARGET=--allow-ap-only ;; mif|mif-cp) TARGET=--allow-mif-sleep ;; *) exit 2 ;; esac
D=$(cd "$(dirname -- "$0")" && pwd)
LIB=/usr/local/libexec/ztos-power
SLEEP=zrn_sleep_init
LOG=/var/lib/ztos-power-test/mif-trial-$(date +%Y%m%dT%H%M%S)-$MODE.log
mkdir -p "$(dirname "$LOG")"
exec >"$LOG" 2>&1
echo "mif-trial $MODE on $(uname -v)"

[ "$(cat /sys/class/sec/sensorhub/ready)" = 1 ] || { echo "sensor hub not ready"; exit 1; }
[ -f /sys/module/zrn_hotplug/parameters/suspend_cluster_off ] ||
	{ echo "needs the suspend-aware zrn-hotplug.ko (install it, then reboot)"; exit 1; }
[ -d /sys/module/zrn_exynos_rtc ] || insmod "$D/zrn-exynos-rtc.ko"
python3 "$D/initialize-calendar.py"
if [ ! -d /sys/module/$SLEEP ]; then
	S=$(awk '$3 == "cpu_suspend" { print $1 }' /proc/kallsyms)
	R=$(awk '$3 == "cpu_resume" { print $1 }' /proc/kallsyms)
	insmod "$D/zrn-sleep-init.ko" enable_deep=1 dry_run=1 prepare_only=0 \
		eint_wake=0xffefffff suspend_enter=0x$S resume=0x$R
	echo s2idle > /sys/power/mem_sleep
fi
rm -f /var/lib/zrn-cpuidle/loading
[ -d /sys/module/zrn_cpuidle ] || ZRN_CPUIDLE_CPUS=0xff ZRN_CPUIDLE_SICD=0 ZRN_CPUIDLE_RELAX=0 \
	ZRN_CPUIDLE_S2IDLE_SICD=0 /usr/local/sbin/zrn-cpuidle try
# ACPM's TMU plugin, which Samsung's kernel attaches at boot and ours did not
python3 "$D/load-acpm-tmu.py" "$D/exynos9810_acpm_tmu.fw" $LIB/s9p-acpm
# Buck ramp rates as Samsung's regulator driver leaves them: every buck has
# regulator-ramp-delay = <6000> in the stock tree, code 0 (6 mV/us) in
# BUCKRAMP (0x3f) and BUCKRAMP2 (0x40). Mainline has no S2MPS18 driver and
# keeps the reset values 0x45/0x15 (12 mV/us on vdd_mif, vdd_int, the
# memory rails and others).
python3 $LIB/s9p-acpm write 1 63 0
python3 $LIB/s9p-acpm write 1 64 0
P=/sys/module/$SLEEP/parameters
echo "init: changes $(cat $P/init_changes), verified $(cat $P/init_verified)"

UDC=$(find /sys/kernel/config/usb_gadget -name UDC | head -1)
CONTROLLER=$(cat "$UDC")
finish() {
	echo 1 > $P/dry_run
	echo 0 > $P/mif_sleep
	echo 0 > /sys/power/pm_debug_messages
	echo Y > /sys/module/printk/parameters/console_suspend
	printf '%s' "$CONTROLLER" > "$UDC"
	modprobe snd-zrn-abox || true
	ztos_as_user systemctl --user start pipewire.socket pipewire.service wireplumber.service || true
	systemctl start ztoscorenetworking-modem.service ztoscorenetworking.service || true
	echo "mif-trial $MODE finished"
	sync
}
trap finish EXIT
# memory sleeps only when every master lets go of it: the modem and audio too
if [ "$MODE" != mif-cp ]; then
	systemctl stop ztoscorenetworking.service ztoscorenetworking-modem.service
	python3 - <<'PY'
import os, fcntl
fd = os.open('/dev/umts_boot0', os.O_RDWR)
fcntl.ioctl(fd, 0x6f20, 0)  # modem power off
os.close(fd)
PY
fi
ztos_as_user systemctl --user stop wireplumber.service pipewire.service pipewire.socket
rmmod snd_zrn_abox
printf 'screen off\n' > "$ZTOS_RUNTIME_DIR/ztmetro.ctl"
sleep 2
echo 0 > $P/dry_run
# every message up to the firmware call into the console log, and into
# zrn-lastlog's memory, which TWRP leaves alone: after a hang, insmod
# zrn-lastlog.ko again and read /proc/zrn-lastlog-previous
[ -d /sys/module/zrn_lastlog ] || insmod "$D/zrn-lastlog.ko"
echo N > /sys/module/printk/parameters/console_suspend
echo 1 > /sys/power/pm_debug_messages
echo 8 > /proc/sys/kernel/printk
sync
printf '\n' > "$UDC"
ZRN_SYSTEM_SLEEP_MODULE=$SLEEP python3 -u $LIB/suspend-test.py --mem-sleep deep \
	--wake-source rtc --seconds ${TRIAL_SECONDS:-10} --connected-diagnostic --ignore-host-req-wake \
	--acpm-fvp-tool $LIB/s9p-acpm --mif-suspend-khz ${MIF_SUSPEND_KHZ:-421000} --mif-restore-khz 1794000 \
	--int-suspend-khz 107000 --int-restore-khz 534000 $TARGET
