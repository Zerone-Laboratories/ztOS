#!/bin/bash
# G1 (root): a GNSS reader streams; `once 60 --ignore-inhibit timed-gnss`;
# then the reader leaves and a plain `once 60` runs with GPS off.
H=/home/zerone/team-r2-holes; R=$H/g1.txt; P=/sys/module/zrn_sleep_init/parameters
start=$(date +%s)
pids() { echo "lhd=$(pgrep -x lhd) csf=$(pgrep -x ztoscsf) sensord=$(pgrep -x ztoscsf-sensord) gpsd=$(pgrep -x gpsd)"; }
st() { echo "clear_grp4=$(cat $P/clear_grp4) entries=$(cat $P/entries) ready=$(cat /sys/class/sec/sensorhub/ready) hub_refused=$(cat $P/hub_refused) grp4_stale_clears=$(cat $P/grp4_stale_clears)"; }
echo "$(date +%T) G1 start $(pids) $(st)" > $R
python3 /home/zerone/team-r2/r2-reader.py 260 $H/g1-reader.out &
RD=$!
sleep 40
echo "$(date +%T) reader up: gnss=$(systemctl is-active ztos-gnss) inhibits=$(ls /run/zrn-sleepd/inhibit | tr '\n' ,) $(pids)" >> $R
for try in 1 2 3 4; do
	/usr/local/sbin/zrn-sleepd once 60 --ignore-inhibit timed-gnss --ignore-inhibit timed-push
	last=$(grep -a 'once: woke' /var/lib/zrn-sleep/log | tail -1)
	echo "$(date +%T) try $try: ${last:20}" >> $R
	echo "$last" | grep -q cancelled || break
	sleep 2
done
sleep 60
echo "$(date +%T) 60 s after: $(pids) $(st)" >> $R
J=$(journalctl --since @$start --no-pager -o cat)
echo "$(date +%T) journal: MCU_REQ_RESP=$(grep -c MCU_REQ_RESP <<<"$J") hubreset=$(grep -c 'zrn-bbd: hub reset' <<<"$J") ESW=$(grep -c 'ESW:CRASHED' <<<"$J") Assert=$(grep -c 'Assert=' <<<"$J")" >> $R
grep -a -E 'firmware gave|once: woke|cancelled|refused' /var/lib/zrn-sleep/log | tail -6 | cut -c1-230 >> $R
journalctl --since @$start --no-pager -o short-monotonic | grep -E 'zrn-system-sleep: (handoff|smc|refused)|PM: suspend (entry|exit)' | cut -c1-200 >> $R
kill $RD 2>/dev/null
for i in $(seq 30); do journalctl --since @$start --no-pager -u ztos-gnss-gate -o cat | grep -q 'no readers; GPS off' && break; sleep 2; done
for i in $(seq 30); do systemctl is-active -q ztos-gnss || break; sleep 2; done
echo "$(date +%T) reader gone: gnss=$(systemctl is-active ztos-gnss) inhibits=$(ls /run/zrn-sleepd/inhibit | tr '\n' ,)" >> $R
sleep 5
/usr/local/sbin/zrn-sleepd once 60 --ignore-inhibit timed-push
echo "$(date +%T) GPS off: $(grep -a 'once: woke' /var/lib/zrn-sleep/log | tail -1 | cut -c21-) $(pids) $(st)" >> $R
sleep 20
J=$(journalctl --since @$start --no-pager -o cat)
echo "$(date +%T) end journal: MCU_REQ_RESP=$(grep -c MCU_REQ_RESP <<<"$J") hubreset=$(grep -c 'zrn-bbd: hub reset' <<<"$J") ESW=$(grep -c 'ESW:CRASHED' <<<"$J") Assert=$(grep -c 'Assert=' <<<"$J")" >> $R
echo DONE >> $R
