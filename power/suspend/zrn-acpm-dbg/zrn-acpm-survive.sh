#!/bin/sh
# Does ACPM's flexpmu debug block (0x0204F800) survive a reset?
# Read it, do one AP-only sleep (zrn-sleepd once 10, the installed path that
# wakes), read it again, then soft-reboot. After the reboot read it a third
# time with insmod zrn_acpm_dbg.ko; compare the three in the journal:
#   journalctl -k -b -1 | grep zrn-acpm-dbg   (marks: before / after-sleep)
# Run detached (Wi-Fi may not come back after SoC sleep):
#   systemd-run --unit=zrn-acpm-survive /usr/local/libexec/ztos-power/zrn-acpm-survive.sh
D=/usr/local/libexec/ztos-power
echo "zrn-acpm-survive: before" > /dev/kmsg
insmod $D/zrn_acpm_dbg.ko 2>/dev/null
# the push and hub timers inhibit sleep on a bench phone; skip them for this one sleep only
/usr/local/sbin/zrn-sleepd once 10 --ignore-inhibit timed-push --ignore-inhibit timed-hub
echo "zrn-acpm-survive: after-sleep rc=$?" > /dev/kmsg
insmod $D/zrn_acpm_dbg.ko 2>/dev/null
echo "zrn-acpm-survive: rebooting" > /dev/kmsg
sleep 5
systemctl reboot
