#!/bin/bash
# J1 rerun (root): GPS off, once 60 and once 300; after each, hub PIDs, a
# reader's first fix, crash lines
H=/home/zerone/team-r2-holes; R=$H/j1.txt; : > $R
start=$(date +%s)
pids() { echo "lhd=$(pgrep -x lhd) sensord=$(pgrep -x ztoscsf-sensord) ready=$(cat /sys/class/sec/sensorhub/ready)"; }
for s in 60 300; do
	for i in $(seq 60); do systemctl is-active -q ztos-gnss || break; sleep 2; done
	echo "$(date +%T) [$s] before: $(pids) gnss=$(systemctl is-active ztos-gnss)" >> $R
	/usr/local/sbin/zrn-sleepd once $s --ignore-inhibit timed-push
	echo "$(date +%T) [$s] $(grep -a 'once: woke' /var/lib/zrn-sleep/log | tail -1 | cut -c21-)" >> $R
	python3 /home/zerone/team-r2/r2-reader.py 45 $H/j1-$s.out
	echo "$(date +%T) [$s] after: $(pids) fix: $(python3 /home/zerone/team-r2/r2-fix.py $H/j1-$s.out 0)" >> $R
	sleep 15
done
J=$(journalctl --since @$start --no-pager -o cat)
echo "$(date +%T) journal: MCU_REQ_RESP=$(grep -c MCU_REQ_RESP <<<"$J") hubreset=$(grep -c 'zrn-bbd: hub reset' <<<"$J") ESW=$(grep -c 'ESW:CRASHED' <<<"$J") Assert=$(grep -c 'Assert=' <<<"$J") OnCommErr=$(grep -c OnCommunicationError <<<"$J")" >> $R
echo DONE >> $R
