#!/bin/sh
# Run on the phone under systemd so USB loss during suspend is harmless.
exec > /var/tmp/bt-sleep-check.log 2>&1
set -u
date -u
systemctl stop zrn-sleepd
trap 'systemctl start zrn-sleepd' EXIT
bluetoothctl scan off
bluetoothctl show
/usr/local/sbin/zrn-sleepd once 8
printf '\nSleep counters after resume:\n'
cat /sys/module/zrn_sleep_init/parameters/entries
cat /sys/module/zrn_sleep_init/parameters/wake_stat
cat /var/lib/zrn-sleep/log | tail -12
bluetoothctl show
timeout 25 bluetoothctl --timeout 20 scan on
bluetoothctl devices
bluetoothctl scan off
dmesg | grep -Ei 'PM: suspend|PM: resume|zrn-bt|hci0|Oops|Unknown symbol' | tail -60
date -u
