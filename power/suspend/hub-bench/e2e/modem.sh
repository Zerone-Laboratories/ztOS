#!/bin/bash
# GPS off, 3 x `once 300`: CP wakes and channels; Wi-Fi and push after each;
# then the CP's IPC must still answer a 0501 probe. Root, through systemd-run.
. /home/zerone/team-r2-e2e/common.sh; R=$H/modem.txt; : > $R
start=$(date +%s)
log() { echo "$(date +%T.%3N) $*" >> $R; }
shell 'screen off'; sleep 3
for i in $(seq 60); do systemctl is-active -q ztos-gnss || break; sleep 1; done
log "start $(pids) $(par) inhibit=$(inh) uptime=$(bt)"
log "ipc before: last 0501 answers: $(journalctl -b -u ztoscorenetworking --no-pager -o short-monotonic | grep 'ipc <- 0501' | tail -3 | awk '{print $1$2}' | tr '\n' ' ') silent-lines=$(journalctl -b -u ztoscorenetworking --no-pager | grep -c 'CP silent')"
for k in 1 2 3; do
	cat /sys/class/wakeup/*/name 2>/dev/null >/dev/null
	NA=$(next_alarm)
	/usr/local/sbin/zrn-sleepd once 300 --ignore-inhibit timed-push
	WW=$(date +%s); EX=$(exit_epoch)
	log "[m$k] $(lastwoke) | next android wakeup alarm due $NA, suspend exit $EX, exit-due $(d $EX ${NA%% *})s | $(par)"
	log "[m$k] $(wifi_wait) $(push_wait $WW) $(pids)"
	sleep 5
done
log "waiting for a probe answer after the sleeps"
T=$(date +%s)
for i in $(seq 75); do
	journalctl --since @$T -u ztoscorenetworking --no-pager -o cat | grep -q 'ipc <- 0501' && break; sleep 2
done
log "ipc after: 0501 answer since the last sleep: $(journalctl --since @$T -u ztoscorenetworking --no-pager -o short-monotonic | grep 'ipc <- 0501' | head -1 | cut -c1-60) ipc-lines-since-start=$(journalctl --since @$start -u ztoscorenetworking --no-pager | grep -c 'ipc <- ') silent=$(journalctl --since @$start -u ztoscorenetworking --no-pager | grep -c 'CP silent')"
log "woke lines: $(grep -a 'once: woke' /var/lib/zrn-sleep/log | tail -3 | cut -c21- | tr '\n' '|')"
log "end $(jcount $start)"
echo DONE >> $R
