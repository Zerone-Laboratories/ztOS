#!/bin/bash
# GNSS streaming through `once 60`, then a 300 s window of `once 300`s
# (timed-gnss not counted; the gate's rewrite cancels are retried); hub,
# sensors, Wi-Fi and push after. Root, through systemd-run.
. /home/zerone/team-r2-e2e/common.sh; R=$H/gnss.txt; : > $R
start=$(date +%s)
log() { echo "$(date +%T.%3N) $*" >> $R; }
touch /run/ztos-gnss/keep-r2e2e
shell 'screen off'; sleep 3
log "start $(pids) $(par) inhibit=$(inh)"
python3 $H/r2-reader.py 900 $H/gnss-reader.out & RD=$!
sleep 40
log "reader up: gnss=$(systemctl is-active ztos-gnss) inhibit=$(inh) $(pids)"
run() {	# run SECONDS TAG
	local t
	for try in 1 2 3 4 5; do
		/usr/local/sbin/zrn-sleepd once $1 --ignore-inhibit timed-gnss --ignore-inhibit timed-push
		t=$(lastwoke); log "[$2] try $try: $t | $(par)"
		echo "$t" | grep -q cancelled || break; sleep 2
	done
}
run 60 g60; W60=$(bt)
sleep 20
log "[g60] 20 s after: $(pids) gnss=$(systemctl is-active ztos-gnss)"
T300=$(bt); n=0
while [ $(d $(bt) $T300 | cut -d. -f1) -lt 300 ] && [ $n -lt 15 ]; do
	n=$((n+1)); run 300 g300-$n; sleep 2
done
log "[g300] window $(d $(bt) $T300)s, $n onces; $(pids) gnss=$(systemctl is-active ztos-gnss)"
sleep 15
log "reader through: $(python3 $H/r2-fix.py $H/gnss-reader.out $W60)"
log "journal: $(jcount $start)"
journalctl --since @$start --no-pager -o short-monotonic | grep -E 'zrn-system-sleep: (handoff|refused)|PM: suspend (entry|exit)|MCU_REQ_RESP|hub reset|Assert=|ESW|AP_SLEEP' | cut -c1-230 > $H/gnss-kern.txt
kill $RD 2>/dev/null; wait $RD 2>/dev/null
rm -f /run/ztos-gnss/keep-r2e2e
for i in $(seq 40); do systemctl is-active -q ztos-gnss || break; sleep 1; done
log "reader gone: gnss=$(systemctl is-active ztos-gnss) inhibit=$(inh)"
log "$(sensors_fresh)"
WW=$(date +%s); log "$(wifi_wait) $(push_wait $((WW - 30)))"
log "end $(pids) $(par) $(jcount $start)"
echo DONE >> $R
