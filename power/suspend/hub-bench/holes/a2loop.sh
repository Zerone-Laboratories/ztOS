#!/bin/bash
# A2 loop (root): `once 1800` again after each modem wake, until a wake that
# is not the modem's (max 12); the next Android wakeup alarm noted before each
H=/home/zerone/team-r2-holes; R=$H/a2loop.txt
: > $R
for i in $(seq 12); do
	waydroid shell -- dumpsys alarm > $H/a2-$i.txt 2>&1
	echo "$(date +%T.%N | cut -c1-12) [$i] $(grep -E 'Next wakeup alarm' $H/a2-$i.txt | head -1) frozen=$(grep frozen /sys/fs/cgroup/lxc.payload.waydroid/cgroup.events)" >> $R
	/usr/local/sbin/zrn-sleepd once 1800 --ignore-inhibit timed-push
	last=$(grep -a 'once: woke' /var/lib/zrn-sleep/log | tail -1)
	echo "$(date +%T.%N | cut -c1-12) [$i] ${last:20}" >> $R
	grep -a 'alarm wake' /var/lib/zrn-sleep/log | tail -1 >> $R
	echo "$last" | grep -qE 'woke: (modem|cancelled|aborted|alarm-due|hub-busy)' || break
	sleep 3
done
waydroid shell -- dumpsys alarm > $H/a2-after.txt 2>&1
echo DONE >> $R
