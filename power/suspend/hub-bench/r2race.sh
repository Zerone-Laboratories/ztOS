#!/bin/bash
# B4: a GNSS reader connects once zrn-sleepd's step reads 'suspend' (after its
# re-check), 3 runs. Root. Results in ~zerone/team-r2/results.txt
H=/home/zerone/team-r2; R=$H/results.txt
for i in 1 2 3; do
	T=B4-$i; start=$(date +%s)
	for k in $(seq 60); do systemctl is-active -q ztos-gnss || break; sleep 2; done
	sleep 5
	/usr/local/sbin/zrn-sleepd once 60 &
	ONCE=$!
	until grep -q ' suspend' /run/zrn-sleepd/step 2>/dev/null; do sleep 0.02; done
	python3 $H/r2-reader.py 150 $H/reader-$T.out &
	RD=$!
	wait $ONCE
	sleep 75
	J=$(journalctl --since @$start --no-pager -o cat)
	echo "$(date +%T) [$T] once: $(grep -a 'once: woke' /var/lib/zrn-sleep/log | tail -1 | cut -c21-) reader: $(python3 $H/r2-fix.py $H/reader-$T.out 0) MCU_REQ_RESP=$(grep -c MCU_REQ_RESP <<<"$J") hubreset=$(grep -c 'zrn-bbd: hub reset' <<<"$J") refused=$(grep -c 'refused: host_req' <<<"$J") Assert=$(grep -c 'Assert=' <<<"$J") lhd=$(pgrep -x lhd)" >> $R
	echo "$(date +%T) [$T] lines: $(grep -E 'handoff|smc=|refused|AP_SLEEP|sent 0xd3|MCU_REQ_RESP|hub reset|reader arrived|GPS on|sleep cancelled' <<<"$J" | sed 's/zrn-system-sleep: //' | tr '\n' '|' | cut -c1-1500)" >> $R
	kill $RD 2>/dev/null; sleep 20
done
touch $H/b4-done
