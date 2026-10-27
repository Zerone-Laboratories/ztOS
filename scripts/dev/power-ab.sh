#!/bin/bash
# power-ab.sh: what each part of the phone costs while it is "idle", measured by
# the fuel gauge with NO cable in (then the gauge's current is the whole phone).
#
#   run as root on the phone, detached:  setsid -f bash power-ab.sh
#
# It waits for the charger to go away (up to 15 minutes), settles, and then for
# each condition (cumulative: each keeps what the one before switched off) takes
# 20 one-second readings of the battery current. The result is in
# /var/tmp/power-ab.log. Everything is put back at the end, and the phone buzzes
# twice: then plug it in.
#
# Gentle on purpose: processes are paused (SIGSTOP) and continued, not killed;
# nothing is unloaded; no cable-less step takes the phone's only link away.
. /usr/lib/ztos/user.sh
LOG=/var/tmp/power-ab.log
CHG=/sys/class/power_supply/max77705-charger/online
BAT=/sys/class/power_supply/max170xx_battery
CTL=$ZTOS_RUNTIME_DIR/ztmetro.ctl
: > "$LOG"
say() { echo "$*" >> "$LOG"; }
shell() { printf '%s\n' "$1" > "$CTL"; }

say "power-ab $(date)  battery $(cat $BAT/capacity) % $(cat $BAT/voltage_now) uV"
say "waiting for the cable to come out"
n=0
while [ "$(cat $CHG)" != 0 ]; do
	sleep 2; n=$((n + 1))
	[ $n -gt 450 ] && { say "no unplug in 15 minutes: stopped"; exit 1; }
done
say "unplugged at $(date +%T); screen off, settling"
shell "screen off"
sleep 25

reading() {	# label: average battery current (uA, negative = from the battery) over 20 s
	local label=$1 sum=0 v i
	sleep 8
	for i in $(seq 20); do
		v=$(cat $BAT/current_now); sum=$((sum + v)); sleep 1
	done
	say "$(printf '%-34s %7d mA   (%s mV, %s C, %s %%)' "$label" $((sum / 20000)) "$(($(cat $BAT/voltage_now) / 1000))" \
		"$(($(cat $BAT/temp) / 10)).$(($(cat $BAT/temp) % 10))" "$(cat $BAT/capacity)")"
}
pause()  { for p in "$@"; do pkill -STOP -x "$p" 2>/dev/null; done; }
resume() { for p in "$@"; do pkill -CONT -x "$p" 2>/dev/null; done; }

reading "baseline, screen off"
reading "baseline again"

nmcli radio wifi off 2>/dev/null; sleep 2
reading "+ Wi-Fi radio off"

pause lhd zrnsensord
reading "+ sensor hub daemons paused"

pause zrn-modemd
reading "+ modem daemon paused"

pause zrnperfd ztoscsf
reading "+ perfd and the framework paused"

for c in 3 2 1; do echo 0 > /sys/devices/system/cpu/cpu$c/online 2>/dev/null; done
say "online now: $(cat /sys/devices/system/cpu/online)"
reading "+ cpu1-3 offline (cpu0 and 4 left)"

# the display controller: panel to sleep, not only the scanout off
PS=/sys/module/s9p_decon/parameters/panel_sleep
if [ -w $PS ]; then
	was=$(cat $PS); echo Y > $PS
	shell "screen on"; sleep 3; shell "screen off"; sleep 5
	reading "+ panel put to sleep (panel_sleep=Y)"
	echo "$was" > $PS
fi

# put everything back
for c in 1 2 3; do echo 1 > /sys/devices/system/cpu/cpu$c/online 2>/dev/null; done
resume zrnperfd ztoscsf zrn-modemd lhd zrnsensord
nmcli radio wifi on 2>/dev/null
say "online restored: $(cat /sys/devices/system/cpu/online)"
say "done $(date +%T)"
shell "haptic heavy"; sleep 0.6; shell "haptic heavy"
