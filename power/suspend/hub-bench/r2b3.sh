#!/bin/bash
# B2b4 (K1, pulse off), then B3: AP_SLEEP pulse on, 3 GPS-off and 3 GNSS-streaming sleeps
H=/home/zerone/team-r2
$H/r2bench2.sh on 60 B2b4 32768; sleep 20
echo 1 > /sys/module/zrn_bbd/parameters/ap_sleep_pulse
echo "$(date +%T) [B3] ap_sleep_pulse=$(cat /sys/module/zrn_bbd/parameters/ap_sleep_pulse)" >> $H/results.txt
for r in "off 60 B3a" "off 60 B3b" "off 60 B3c" "on 60 B3d" "on 60 B3e" "on 60 B3f"; do $H/r2bench2.sh $r; sleep 20; done
touch $H/b3-done
