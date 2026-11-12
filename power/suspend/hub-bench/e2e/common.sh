# common helpers for the round-2 end-to-end bench (root, on the phone)
H=/home/zerone/team-r2-e2e; P=/sys/module/zrn_sleep_init/parameters; B=/sys/class/sec/sensorhub
CTL=/run/user/1000/ztmetro.ctl; SCTL=/run/ztoscsf-sensord.ctl; INH=/run/zrn-sleepd/inhibit
bt() { awk '{print $1}' /proc/uptime; }
d() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%.1f", a - b}'; }
shell() { timeout 3 sh -c "echo \"$1\" > $CTL" 2>/dev/null; }
sctl() { timeout 3 sh -c "echo \"$1\" > $SCTL" 2>/dev/null; }
pids() { echo "lhd=$(pgrep -x lhd|tr '\n' ,) csf=$(pgrep -x ztoscsf|tr '\n' ,) sensord=$(pgrep -x ztoscsf-sensord) gpsd=$(pgrep -x gpsd) netd=$(pgrep -x ztoscorenetwork|tr '\n' ,) ready=$(cat $B/ready)"; }
par() { for n in clear_grp4 eint_follow eint_effective hub_refused hub_pend_cleared grp4_stale_clears grp4_before grp4_at_smc entries; do printf '%s=%s ' $n "$(cat $P/$n 2>/dev/null)"; done; printf 'resume_seq=%s ap_suspends=%s pulse=%s' "$(cat $B/resume_seq)" "$(cat $B/ap_suspends)" "$(cat /sys/module/zrn_bbd/parameters/ap_sleep_pulse)"; }
inh() { echo "[$(ls $INH 2>/dev/null | tr '\n' ' ')]"; }
jcount() {	# jcount SINCE_EPOCH: hub error counts
	local J; J=$(journalctl --since "@$1" --no-pager -o cat)
	echo "MCU_REQ_RESP=$(grep -c MCU_REQ_RESP <<<"$J") Assert=$(grep -c 'Assert=' <<<"$J") ESW=$(grep -c 'ESW:CRASHED' <<<"$J") OnCommErr=$(grep -c OnCommunicationError <<<"$J") hubreset=$(grep -c 'zrn-bbd: hub reset' <<<"$J") powerretry=$(grep -c 'recovered after power' <<<"$J") noecho=$(grep -c 'no echo' <<<"$J") refused=$(grep -c 'refused: host_req' <<<"$J") pulse=$(grep -c 'AP_SLEEP pulse,' <<<"$J") nopulse=$(grep -c 'no AP_SLEEP pulse' <<<"$J") d3=$(grep -c 'sent 0xd3' <<<"$J") bIsAP=$(grep -c bIsAPSleeping <<<"$J")"
}
lastwoke() { grep -a 'once: woke' /var/lib/zrn-sleep/log | tail -1 | cut -c21-; }
push() { grep -E '^(state|since)=' /run/user/1000/zrn-pushd.status 2>/dev/null | tr '\n' ' '; }
wifi_wait() { local t0=$(bt); for i in $(seq 120); do
	if iw dev wlp1s0 link 2>/dev/null | grep -q '^Connected' && nmcli -t -f DEVICE,STATE dev 2>/dev/null | grep -q '^wlp1s0:connected$'; then echo "wifi +$(d $(bt) $t0)s"; return; fi; sleep 0.5; done; echo "wifi NOT back in 60 s"; }
push_wait() { for i in $(seq 120); do s=$(push); st=${s#state=}; st=${st%% *}; since=$(echo "$s" | grep -oE 'since=[0-9]+' | cut -d= -f2)
	if [ "$st" = online ] && [ "${since:-0}" -ge "$1" ]; then echo "push online +$(( since - $1 ))s"; return; fi; sleep 0.5; done; echo "push not online in 60 s: $(push)"; }
sensors_fresh() {	# screen on, watch accel+prox, time to a fresh reading; then screen off
	local t0 a0 p0 ta tp
	a0=$(stat -c %Y.%y /run/ztoscsf-sensors/accel 2>/dev/null | cut -d. -f1); p0=$(stat -c %Y /run/ztoscsf-sensors/prox 2>/dev/null)
	shell 'screen on'; sleep 1; t0=$(date +%s.%N)
	sctl "watch accel 200"; sctl "watch prox 200"
	ta=; tp=
	for i in $(seq 100); do
		[ -z "$ta" ] && [ "$(stat -c %.3Y /run/ztoscsf-sensors/accel 2>/dev/null | tr -d .)" -gt "$(echo $t0 | cut -c1-14 | tr -d .)" ] 2>/dev/null && ta=$(d $(date +%s.%N) $t0)
		[ -z "$tp" ] && [ "$(stat -c %.3Y /run/ztoscsf-sensors/prox 2>/dev/null | tr -d .)" -gt "$(echo $t0 | cut -c1-14 | tr -d .)" ] 2>/dev/null && tp=$(d $(date +%s.%N) $t0)
		[ -n "$ta" ] && [ -n "$tp" ] && break; sleep 0.1
	done
	echo "sensors after screen on: accel fresh ${ta:-never}s prox fresh ${tp:-never}s light age $(( $(date +%s) - $(stat -c %Y /run/ztoscsf-sensors/light 2>/dev/null || echo 0) ))s ready=$(cat $B/ready)"
	sctl "unwatch accel"; sctl "unwatch prox"; sleep 1
	shell 'screen off'
}
next_alarm() {	# the next Android wakeup alarm: prints "DUE_EPOCH (+in)"
	local l w
	l=$(waydroid shell -- dumpsys alarm 2>/dev/null | grep -m1 'Next wakeup alarm')
	w=$(echo "$l" | grep -oE '[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9:.]+')
	[ -n "$w" ] && echo "$(date -u -d "$w" +%s.%3N) ($(echo "$l" | awk '{print $4}'))" || echo "none"
}
exit_epoch() { journalctl -k --since "-30min" --no-pager -o short-unix | grep 'PM: suspend exit' | tail -1 | awk '{print $1}'; }
