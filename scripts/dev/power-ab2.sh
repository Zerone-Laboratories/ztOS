#!/bin/bash
# power-ab2.sh: the second round of idle-drain measurements (see power-ab.sh).
# Unplugged, screen off, cumulative: the display panel asleep is the reference, then
# Wi-Fi radio, sensor hub, airplane mode, UFS runtime power management, the touch
# controller, the USB controller and the Wi-Fi chip's PCIe link. Run as root, detached.
# Result: /var/tmp/power-ab2.log. Everything is restored; the blue light shows when done.
. /usr/lib/ztos/user.sh
LOG=/var/tmp/power-ab2.log
CHG=/sys/class/power_supply/max77705-charger/online
BAT=/sys/class/power_supply/max170xx_battery
CTL=$ZTOS_RUNTIME_DIR/ztmetro.ctl
UFS=/sys/devices/platform/soc@0/11120000.ufs
: > "$LOG"
say() { echo "$*" >> "$LOG"; }
shell() { printf '%s\n' "$1" > "$CTL"; }
say "power-ab2 $(date)  battery $(cat $BAT/capacity) %"
n=0
while [ "$(cat $CHG)" != 0 ]; do sleep 2; n=$((n + 1)); [ $n -gt 450 ] && { say "no unplug: stopped"; exit 1; }; done
say "unplugged $(date +%T)"
PS=/sys/module/s9p_decon/parameters/panel_sleep
was=$(cat $PS); echo Y > $PS
shell "screen on"; sleep 3; shell "screen off"; sleep 25

reading() {
	local label=$1 sum=0 v i
	sleep 8
	for i in $(seq 20); do v=$(cat $BAT/current_now); sum=$((sum + v)); sleep 1; done
	say "$(printf '%-38s %7d mA   (%s C, %s %%)' "$label" $((sum / 20000)) "$(($(cat $BAT/temp) / 10))" "$(cat $BAT/capacity)")"
}
reading "panel asleep (reference)"
reading "again"
nmcli radio wifi off 2>/dev/null; sleep 2
reading "+ Wi-Fi radio off"
pkill -STOP -x lhd; pkill -STOP -x zrnsensord
reading "+ sensor hub daemons paused"
printf 'airplane on\n' > /run/zrn-modem.ctl; sleep 6
reading "+ airplane mode (radio off)"
for d in $UFS/host0/target0:0:0/0:0:0:*/power $UFS/host0/target0:0:0/power $UFS/host0/power $UFS/power; do
	echo auto > $d/control 2>/dev/null
done
sleep 4
reading "+ UFS runtime power management"
echo 0-0048 > /sys/bus/i2c/drivers/s6sy761/unbind 2>/dev/null
reading "+ touch controller unbound"
UDC=$(ls /sys/kernel/config/usb_gadget/*/UDC | head -1); udc=$(cat $UDC); echo "" > $UDC 2>/dev/null
reading "+ USB controller unbound"
echo 1 > /sys/bus/pci/devices/0000:01:00.0/remove 2>/dev/null; sleep 3
reading "+ Wi-Fi chip removed from PCIe"

# put everything back
echo 1 > /sys/bus/pci/rescan 2>/dev/null; sleep 4
echo "$udc" > $UDC 2>/dev/null
echo 0-0048 > /sys/bus/i2c/drivers/s6sy761/bind 2>/dev/null
for d in $UFS/host0/target0:0:0/0:0:0:*/power; do echo on > $d/control 2>/dev/null; done
printf 'airplane off\n' > /run/zrn-modem.ctl
pkill -CONT -x lhd; pkill -CONT -x zrnsensord
nmcli radio wifi on 2>/dev/null
echo "$was" > $PS
say "restored; wifi device: $(ls /sys/bus/pci/devices | tr '\n' ' ')  usb: $(cat $UDC)  touch: $(readlink /sys/bus/i2c/devices/0-0048/driver | xargs -r basename)"
say "done $(date +%T)"
# the blue light for 40 s: finished, plug it in
echo none > /sys/class/leds/blue:status/trigger 2>/dev/null
echo 255 > /sys/class/leds/blue:status/brightness 2>/dev/null; sleep 40; echo 0 > /sys/class/leds/blue:status/brightness 2>/dev/null
