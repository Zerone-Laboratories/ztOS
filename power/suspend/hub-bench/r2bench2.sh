#!/bin/bash
# r2bench.sh off|on SECONDS TAG [FOLLOW]: one SoC sleep (zrn-sleepd once), GNSS off or
# streaming with the timed-gnss inhibit bypassed; results in ~zerone/team-r2/results.txt.
# FOLLOW sets zrn-sleep-init's eint_follow first (B2 control arm). Run as root.
M=$1; S=${2:-60}; T=${3:-x}; F=$4; H=/home/zerone/team-r2; R=$H/results.txt
P=/sys/module/zrn_sleep_init/parameters; B=/sys/class/sec/sensorhub
start=$(date +%s); up() { cut -d' ' -f1 /proc/uptime; }
pids() { echo "lhd=$(pgrep -x lhd) sensord=$(pgrep -x ztoscsf-sensord) gpsd=$(pgrep -x gpsd)"; }
par() { for n in eint_follow eint_effective hub_refused hub_pend_cleared grp4_stale_clears grp4_at_smc entries; do printf '%s=%s ' $n "$(cat $P/$n 2>/dev/null)"; done; printf 'resume_seq=%s ap_suspends=%s pulse=%s' "$(cat $B/resume_seq 2>/dev/null)" "$(cat $B/ap_suspends 2>/dev/null)" "$(cat /sys/module/zrn_bbd/parameters/ap_sleep_pulse 2>/dev/null)"; }
[ -n "$F" ] && [ -e $P/eint_follow ] && echo $F > $P/eint_follow
BP=
if [ "$M" = off ]; then
	for i in $(seq 100); do systemctl is-active -q ztos-gnss || break; sleep 2; done
	sleep 5
else
	python3 $H/r2-reader.py $((S + 200)) $H/reader-$T.out &
	sleep 40
	( while :; do rm -f /run/zrn-sleepd/inhibit/timed-gnss; sleep 0.02; done ) & BP=$!
fi
echo "$(date +%T) [$T] mode=$M S=$S gnss=$(systemctl is-active ztos-gnss) $(pids) ready=$(cat $B/ready) $(par)" >> $R
for try in 1 2 3; do /usr/local/sbin/zrn-sleepd once $S; tail -1 /var/lib/zrn-sleep/log | grep -aq "cancelled" || break; echo "$(date +%T) [$T] once cancelled (bench race), again" >> $R; sleep 2; done
woke=$(up)
[ -n "$BP" ] && kill $BP
sleep 3
echo "$(date +%T) [$T] once: $(grep -a 'once: woke' /var/lib/zrn-sleep/log | tail -3 | cut -c21- | tr '\n' '|') $(pids) ready=$(cat $B/ready) $(par)" >> $R
if [ "$M" = off ]; then
	python3 $H/r2-reader.py 40 $H/reader-$T.out
	echo "$(date +%T) [$T] reader after: $(python3 $H/r2-fix.py $H/reader-$T.out 0)" >> $R
	sleep 20
else
	sleep 65
	echo "$(date +%T) [$T] reader through: $(python3 $H/r2-fix.py $H/reader-$T.out $woke)" >> $R
fi
J=$(journalctl --since @$start --no-pager -o cat)
echo "$(date +%T) [$T] journal: MCU_REQ_RESP=$(grep -c MCU_REQ_RESP <<<"$J") Assert=$(grep -c 'Assert=' <<<"$J") ESW=$(grep -c 'ESW:CRASHED' <<<"$J") hubreset=$(grep -c 'zrn-bbd: hub reset' <<<"$J") refused=$(grep -c 'refused: host_req' <<<"$J") pulse=$(grep -c 'AP_SLEEP pulse,' <<<"$J") d3=$(grep -c 'sent 0xd3' <<<"$J") bIsAP=$(grep -c 'bIsAPSleeping' <<<"$J")" >> $R
echo "$(date +%T) [$T] lines: $(grep -E 'handoff|zrn-system-sleep: smc=|refused|AP_SLEEP|no AP_SLEEP|sent 0xd3|no echo|MCU_REQ_RESP|hub reset|bbd_suspend|sensor hub' <<<"$J" | sed 's/zrn-system-sleep: //' | tr '\n' '|' | cut -c1-1800)" >> $R
journalctl --since @$start --no-pager -o short-monotonic | grep -E 'bbd_suspend (\+\+|--)' > $H/bbdsusp-$T.txt
touch $H/done-$T
