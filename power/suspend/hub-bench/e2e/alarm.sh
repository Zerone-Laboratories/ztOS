#!/bin/bash
# An Android *_WAKEUP alarm from sleep: note the next one (and zrn-pushd's
# heartbeat alarm), `once 1800`; again after a wake that is not Android's
# (max 4). exit-due: PM suspend exit minus the Android alarm's due time.
. /home/zerone/team-r2-e2e/common.sh; R=$H/alarm.txt; : >> $R
shell 'screen off'; sleep 3
pushd_due() { local p v; p=$(pgrep -x zrn-pushd | head -1); for f in /proc/$p/fdinfo/*; do grep -q '^clockid:[[:space:]]*9$' $f 2>/dev/null || continue; v=$(awk '/it_value/{gsub(/[(,)]/," "); print $2}' $f); echo $(( $(date +%s) + v )); return; done; echo none; }
for i in 1 2 3 4 5 6; do
	NA=$(next_alarm); PD=$(pushd_due)
	echo "$(date +%T.%3N) [$i] android due $NA, pushd heartbeat due $PD, frozen=$(grep frozen /sys/fs/cgroup/lxc.payload.waydroid/cgroup.events 2>/dev/null)" >> $R
	/usr/local/sbin/zrn-sleepd once 1800 --ignore-inhibit timed-push
	last=$(lastwoke); EX=$(exit_epoch)
	echo "$(date +%T.%3N) [$i] $last | suspend exit $EX, exit-android $(d $EX ${NA%% *})s exit-pushd $(d $EX $PD)s" >> $R
	grep -a 'alarm wake' /var/lib/zrn-sleep/log | tail -1 | cut -c1-120 >> $R
	: # an RTC wake: once's own 300 s cap; go on
	dd=$(d $EX ${NA%% *}); dd=${dd#-}; [ "${dd%%.*}" -lt 10 ] 2>/dev/null && break
	sleep 3
done
waydroid shell -- dumpsys alarm > $H/alarm-after.txt 2>&1
echo DONE >> $R
