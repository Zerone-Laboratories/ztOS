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
# STOCK=1: as close to the stock kernel's SYS_SLEEP entry as we know how
# (build/power-analysis/regdump-20261108T095737Z/stock-exact-plan/PLAN.md and
# the research-*.md next to it). Wake on the PMIC RTC alarm through gpa0-1
# level-low with the stock pull-up, stock wake masks (SoC RTC masked), TMU
# off + AP_SUSPEND, stock devfreq order, no ACPM DRAM log pointer, MIF_en
# never written, cable out. It overrides the per-variable knobs below.
STOCK=${STOCK:-0}
if [ "$STOCK" = 1 ]; then
	TMU_IPC=1 ACPMLOG=0 TRIAL_SECONDS=${TRIAL_SECONDS:-60} MIF_SUSPEND_KHZ=421000
	FLT=1 PDLOG=1 BTS=1 PREP=1 DOMS=1 RTCTICK=0 NOFVP=0
	unset GUARD EINTW PMICEINT PMICRTC ACPMSKIP KEEPMIF GICC DPUOFF
	EINT_WAKE=0xffeffffd
	SLEEP_PARAMS="stock_masks=1 pmic_eint=1 pmic_eint_level=1 pmic_eint_pull=1 \
eint_pend_clear=0 stock_init_extra=1 keep_mif_en=1 eint_filter_analog=1 pd_log=1 \
eint_quiet=0 grp4_stale_clear=0 clear_grp4=0 gicc_off=0 wdt_apm_guard_s=0 usbl2=0"
fi
mkdir -p "$(dirname "$LOG")"
exec >"$LOG" 2>&1
echo "mif-trial $MODE on $(uname -v)"

# SKIPHUB=1 (snapshot runs only): go on without the sensor hub
[ "${SKIPHUB:-0}" = 1 ] || [ "$(cat /sys/class/sec/sensorhub/ready)" = 1 ] || { echo "sensor hub not ready"; exit 1; }
[ -f /sys/module/zrn_hotplug/parameters/suspend_cluster_off ] ||
	{ echo "needs the suspend-aware zrn-hotplug.ko (install it, then reboot)"; exit 1; }
[ -d /sys/module/zrn_exynos_rtc ] || insmod "$D/zrn-exynos-rtc.ko"
python3 "$D/initialize-calendar.py"
if [ ! -d /sys/module/$SLEEP ]; then
	S=$(awk '$3 == "cpu_suspend" { print $1 }' /proc/kallsyms)
	R=$(awk '$3 == "cpu_resume" { print $1 }' /proc/kallsyms)
	insmod "$D/zrn-sleep-init.ko" enable_deep=1 dry_run=1 prepare_only=0 \
		eint_wake=${EINT_WAKE:-0xffefffff} suspend_enter=0x$S resume=0x$R \
		${SLEEP_PARAMS:-}
	echo s2idle > /sys/power/mem_sleep
fi
rm -f /var/lib/zrn-cpuidle/loading
[ -d /sys/module/zrn_cpuidle ] || ZRN_CPUIDLE_CPUS=0xff ZRN_CPUIDLE_SICD=0 ZRN_CPUIDLE_RELAX=0 \
	ZRN_CPUIDLE_S2IDLE_SICD=0 /usr/local/sbin/zrn-cpuidle try
# ACPM's TMU plugin, which Samsung's kernel attaches at boot and ours did not
python3 "$D/load-acpm-tmu.py" "$D/exynos9810_acpm_tmu.fw" $LIB/s9p-acpm
# ...and the messages it sends the plugin: TMU_IPC_INIT once at boot,
# AP_SUSPEND before the sleep (rsvd = cp net connected) and AP_RESUME after
# (exynos_acpm_tmu.c). Ours sent none, so the plugin was never told the AP
# sleeps. TMU_IPC=0 leaves them out, as every trial before 2026-10-29.
# Word 1 of a request: type | rsvd << 8 | tzid << 16.
TMU_IPC=${TMU_IPC:-1}
tmu() { [ "$TMU_IPC" = 1 ] && python3 $LIB/s9p-acpm ipc 12 0 "$1" 0 0 || true; }
tmu 0x01
# ACPM's sleep-sequence log into DRAM that survives a reset (0xafffe000 is
# ACPM 0x8fffe000): after a hang read it with acpmlogptr.py dump 0xafffe000
# STOCK=1 (ACPMLOG=0) leaves the pointer where ACPM put it: stock never moves
# it, and it is the only thing that makes ACPM write DRAM while MIF is down.
if [ "${ACPMLOG:-1}" = 1 ] && [ -f "$D/acpm-log/acpmlogptr.py" ]; then
	python3 "$D/acpm-log/acpmlogptr.py" clear ${LOGPHYS:-0xafffe000}
	python3 "$D/acpm-log/acpmlogptr.py" set ${LOGACPM:-0x8fffe000}
fi
# Buck ramp rates as Samsung's regulator driver leaves them: every buck has
# regulator-ramp-delay = <6000> in the stock tree, code 0 (6 mV/us) in
# BUCKRAMP (0x3f) and BUCKRAMP2 (0x40). Mainline has no S2MPS18 driver and
# keeps the reset values 0x45/0x15 (12 mV/us on vdd_mif, vdd_int, the
# memory rails and others).
python3 $LIB/s9p-acpm write 1 63 0
python3 $LIB/s9p-acpm write 1 64 0
P=/sys/module/$SLEEP/parameters
[ -n "${GUARD:-}" ] && echo $GUARD > $P/wdt_apm_guard_s
[ "${FLT:-0}" = 1 ] && echo Y > $P/eint_filter_analog
[ "${PDLOG:-0}" = 1 ] && echo Y > $P/pd_log
[ -n "${EINTW:-}" ] && echo $EINTW > $P/eint_wake
[ "${PMICEINT:-0}" = 1 ] && echo Y > $P/pmic_eint
[ "${ACPMSKIP:-0}" = 1 ] && python3 $D/acpmskip.py on
[ "${KEEPMIF:-0}" = 1 ] && echo Y > $P/keep_mif_en
[ "${GICC:-0}" = 1 ] && echo Y > $P/gicc_off
# SNAPSHOT=1: read the zrn-snapshot registers at the firmware handoff and give
# the sleep up there (no hang); compare /proc/zrn-sleep-snapshot with TWRP's
[ "${SNAPSHOT:-0}" = 1 ] && echo Y > $P/snapshot_abort
echo "init: changes $(cat $P/init_changes), verified $(cat $P/init_verified)"

UDC=$(find /sys/kernel/config/usb_gadget -name UDC | head -1)
CONTROLLER=$(cat "$UDC")
# Stock's exynos_tmu_suspend (exynos_tmu.c:1670-1697) first turns each TMU
# off (CONTROL bits 0 CORE_EN and 12 TRIP_EN, INTEN of its sensors to 0),
# then the last one sends AP_SUSPEND; resume sends AP_RESUME, then turns them
# on. BIG/LITTLE share 0x10070000 (sensors 1-6), G3D/ISP 0x10074000 (1-2).
tmu_hw() { python3 - "$1" <<'PY'
import json, mmap, os, struct, sys
REGS = {0x10070000: [0x20, 0x120, 0x130, 0x140, 0x150, 0x310, 0x320],
        0x10074000: [0x20, 0x120, 0x130]}
STATE = '/run/zrn-tmu-saved.json'
fd = os.open('/dev/mem', os.O_RDWR | os.O_SYNC)
def io(base, off, val=None):
    m = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=base)
    if val is not None:
        struct.pack_into('<I', m, off, val)
    v = struct.unpack_from('<I', m, off)[0]
    m.close()
    return v
if sys.argv[1] == 'off':
    saved = {f'{b:#x}+{o:#x}': io(b, o) for b, offs in REGS.items() for o in offs}
    json.dump(saved, open(STATE, 'w'))
    for b, offs in REGS.items():
        io(b, 0x20, io(b, 0x20) & ~((1 << 12) | 1))
        for o in offs[1:]:
            io(b, o, 0)
    print('tmu off, saved', saved)
else:
    saved = json.load(open(STATE))
    for key, v in saved.items():       # INTEN first, CONTROL last
        b, o = (int(x, 16) for x in key.split('+'))
        if o != 0x20:
            io(b, o, v)
    for b in REGS:
        io(b, 0x20, saved[f'{b:#x}+0x20'])
    print('tmu restored', {k: hex(io(*(int(x, 16) for x in k.split('+')))) for k in saved})
PY
}
finish() {
	[ "${TMU_SUSPENDED:-0}" = 1 ] && tmu 0x10
	[ "${TMU_HW_OFF:-0}" = 1 ] && tmu_hw restore
	echo 1 > $P/dry_run
	echo 0 > $P/mif_sleep
	echo 0 > /sys/power/pm_debug_messages
	echo Y > /sys/module/printk/parameters/console_suspend
	printf '%s' "$CONTROLLER" > "$UDC"
	:
	ztos_as_user systemctl --user start pipewire.socket pipewire.service wireplumber.service || true
	systemctl start ztoscorenetworking-modem.service ztoscorenetworking.service || true
	[ -f /run/zrn-rtccon ] && python3 $D/rtcw.py on
	if [ "${PREP:-0}" = 1 ]; then [ -n "${UFSOLD:-}" ] && echo $UFSOLD > /sys/devices/platform/soc@0/11120000.ufs/spm_lvl; [ -e /sys/bus/pci/drivers/brcmfmac/0000:01:00.0 ] || echo 0000:01:00.0 > /sys/bus/pci/drivers/brcmfmac/bind || true; fi
	[ -n "${PMICRTC:-}" ] && python3 $D/pmic-rtc-alarm.py disarm
	[ -f /run/zrn-acpmskip.json ] && python3 $D/acpmskip.py off
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
: # no rmmod (standing rule)
printf 'screen off\n' > "$ZTOS_RUNTIME_DIR/ztmetro.ctl"
sleep 2
[ "${DPUOFF:-0}" = 1 ] && python3 $D/pd-ab.py --keep DPU
if [ "${BTS:-0}" = 1 ]; then python3 $D/btsleep.py || true; fi
if [ "${PREP:-0}" = 1 ]; then ip link set wlp1s0 down || true; echo 0000:01:00.0 > /sys/bus/pci/drivers/brcmfmac/unbind || true; UFSOLD=$(cat /sys/devices/platform/soc@0/11120000.ufs/spm_lvl); echo 5 > /sys/devices/platform/soc@0/11120000.ufs/spm_lvl; echo 1 > /sys/module/zrn_hotplug/parameters/suspend_cluster_off; sleep 3; fi
if [ "${DOMS:-0}" = 1 ]; then python3 $D/unused-domains.py --apply || true; python3 $D/pd-ab.py --keep ISPPRE ISPHQ ISPLP IVA || true; fi
echo 0 > $P/dry_run
# every message up to the firmware call into the console log, and into
# zrn-lastlog's memory, which TWRP leaves alone: after a hang, insmod
# zrn-lastlog.ko again and read /proc/zrn-lastlog-previous
[ -d /sys/module/zrn_lastlog ] || insmod "$D/zrn-lastlog.ko"
[ "${LOUD:-1}" = 1 ] && echo N > /sys/module/printk/parameters/console_suspend
echo 1 > /sys/power/pm_debug_messages
[ "${LOUD:-1}" = 1 ] && echo 8 > /proc/sys/kernel/printk || echo 4 > /proc/sys/kernel/printk
sync
[ "${RTCTICK:-1}" = 0 ] && python3 $D/rtcw.py off
[ -n "${PMICRTC:-}" ] && python3 $D/pmic-rtc-alarm.py arm $PMICRTC
printf '\n' > "$UDC"
if [ "$STOCK" = 1 ]; then TMU_HW_OFF=1; tmu_hw off; fi
if [ "$MODE" = mif-cp ]; then tmu 0x0104; else tmu 0x04; fi
TMU_SUSPENDED=1
if [ "${NOFVP:-0}" = 1 ]; then FVPARGS=; else FVPARGS="--acpm-fvp-tool $LIB/s9p-acpm --mif-suspend-khz ${MIF_SUSPEND_KHZ:-421000} --mif-restore-khz 1794000 --int-suspend-khz 107000 --int-restore-khz 534000"; fi
if [ "$STOCK" = 1 ]; then
	# stock sleeps cable-out (dwc3-otg holds a wakelock while a PC is
	# connected) and wakes on the PMIC RTC through EINT gpa0-1; the power key
	# (EINT bit 20) stays open as the backup
	WAKE="--wake-source pmic --pmic-rtc-tool $D/pmic-rtc-alarm.py --stock-devfreq --battery-only --wait-unplug 300"
else
	WAKE="--wake-source rtc --connected-diagnostic"
fi
ZRN_SYSTEM_SLEEP_MODULE=$SLEEP python3 -u $LIB/suspend-test.py --mem-sleep deep \
	$WAKE --seconds ${TRIAL_SECONDS:-10} ${SKIPHUB:+--allow-unready-sensorhub} \
	$FVPARGS $TARGET
