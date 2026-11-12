#!/bin/bash
# The late-inhibit race, N runs (default 10), helper delay 0-MAX s (0.12).
. /home/zerone/team-r2-e2e/common.sh; R=$H/race.txt; : > $R
N=${1:-10}; MAX=${2:-0.12}; start=$(date +%s)
shell 'screen off'; sleep 3
for n in $(seq $N); do
	rm -f $INH/timed-r2
	python3 $H/race-helper.py $((100 + n)) $MAX > $H/race-$n.helper 2>&1 & hp=$!
	/usr/local/sbin/zrn-sleepd once 60 --ignore-inhibit timed-push > $H/race-$n.once 2>&1
	wait $hp; rm -f $INH/timed-r2
	echo "$(date +%T) run $n: $(head -1 $H/race-$n.helper | cut -c1-60) | $(grep -a -E 'once: woke|cancelled' /var/lib/zrn-sleep/log | tail -2 | cut -c21- | tr '\n' '|')" >> $R
	sleep 3
done
journalctl -k --since @$start --no-pager -o short-monotonic | grep -E 'r2: (helper|renamed|step)|zrn-system-sleep: handoff|PM: suspend (entry|exit)' > $H/race-kern.txt
echo DONE >> $R
