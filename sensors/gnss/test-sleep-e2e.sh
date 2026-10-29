#!/bin/bash
# Sleep and GNSS together, end to end, on the S9 (root; run it through
# systemd-run: `zrn-sleepd once` lets the USB gadget go, so SSH drops):
#   systemd-run --unit=zrn-e2e --collect -E PHASES="A B C D E F G" \
#       /path/to/sensors/gnss/test-sleep-e2e.sh
# Results in $OUT/e2e-<phases>.out (default /home/zerone/team-test).
# Phases: A GPS off through a 60 s sleep, GPS and sensors after the wake;
# B a reader through a sleep (must cancel: timed-gnss), then it leaves;
# C a reader leaves with the screen on (idle hold), then a sleep at once;
# D a reader connects while the sleep is at 'settle' (must cancel);
# E a 300 s sleep, GPS after; F kill -9 the gate while it holds the inhibit;
# G a reader over the 10-minute screen-off cap, then the screen on.
# On the charger only: zrn-sleepd's own loop never sleeps there.
# No positions are written: the reader reports times and counts only.
# sensors_check watches accel and prox, and taking them off at screen-off
# asserts the hub's firmware (see test-hub-screen-off.sh): it recovers
# by itself in about 18 s, but phases after A and E see that crash.
H=${OUT:-/home/zerone/team-test}; R=$H/e2e.out; G=$(dirname "$(readlink -f "$0")")
CTL=/run/user/1000/ztmetro.ctl; SCTL=/run/ztoscsf-sensord.ctl
INH=/run/zrn-sleepd/inhibit
PHASES=${PHASES:-A B C D E F}	# G (13 min) on request
log() { echo "$(date +%T.%3N) $*" >> $R; }
d() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%.1f", a - b}'; }
bt() { awk '{print $1}' /proc/uptime; }
shell() { timeout 3 sh -c "echo \"$1\" > $CTL" 2>/dev/null; }
sctl() { timeout 3 sh -c "echo \"$1\" > $SCTL" 2>/dev/null; }
pids() { echo "lhd=$(pgrep -x lhd | tr '\n' ,) hub=$(systemctl show -p MainPID --value ztoscsf-sensorhub) sensord=$(systemctl show -p MainPID --value ztoscsf-sensord) gate=$(systemctl show -p MainPID --value ztos-gnss-gate) ready=$(cat /sys/class/sec/sensorhub/ready)"; }
inh() { echo "[$(ls $INH 2>/dev/null | tr '\n' ' ')]"; }
crash() { journalctl --since "@$1" --no-pager -o short-monotonic 2>/dev/null | grep -E 'OnCommunicationError|ESW:CRASHED|lost readiness|MCU_REQ_RESP|hub reset|recovered after power' ; }
gnss_off() { for i in $(seq 60); do systemctl is-active -q ztos-gnss || return 0; sleep 1; done; return 1; }
wifi_wait() {	# seconds until wlp1s0 is associated and NM connected, from now
	local t0=$(bt)
	for i in $(seq 120); do
		if iw dev wlp1s0 link 2>/dev/null | grep -q '^Connected' &&
		   nmcli -t -f DEVICE,STATE dev 2>/dev/null | grep -q '^wlp1s0:connected$'; then
			echo "wifi associated +$(d $(bt) $t0)s"; return; fi
		sleep 0.5
	done
	echo "wifi NOT back after 60 s"
}
push() { grep -E '^(state|since|reconnects)=' /run/user/1000/zrn-pushd.status 2>/dev/null | tr '\n' ' '; }
push_wait() {	# until pushd is online with since >= $1 (or 60 s)
	for i in $(seq 120); do
		s=$(push); st=${s#state=}; st=${st%% *}; since=$(echo "$s" | grep -oE 'since=[0-9]+' | cut -d= -f2)
		if [ "$st" = online ] && [ "${since:-0}" -ge "$1" ]; then echo "push online +$(( since - $1 ))s ($s)"; return; fi
		sleep 0.5
	done
	echo "push not re-online within 60 s: $(push)"
}
sleep_once() {	# sleep_once SECONDS TAG: one bench sleep, wake facts after it
	local s=$1 tag=$2 before
	before=$(wc -l < /var/lib/zrn-sleep/log)
	log "[$tag] before once $s: $(pids) gnss=$(systemctl is-active ztos-gnss) inhibit=$(inh) push: $(push)"
	/usr/local/sbin/zrn-sleepd once $s
	WAKE_WALL=$(date +%s); WAKE_BT=$(bt)
	log "[$tag] once returned: $(tail -n +$((before + 1)) /var/lib/zrn-sleep/log | cut -c21- | tr '\n' '|')"
	log "[$tag] after: $(pids) entries=$(cat /sys/module/zrn_sleep_init/parameters/entries 2>/dev/null) grp4_before=$(cat /sys/module/zrn_sleep_init/parameters/grp4_before 2>/dev/null) chub_before=$(cat /sys/module/zrn_sleep_init/parameters/chub_status_before 2>/dev/null)"
}
after_wake() {	# after_wake TAG: Wi-Fi, push, the handoff line
	local tag=$1
	log "[$tag] $(wifi_wait)"
	log "[$tag] $(push_wait $WAKE_WALL)"
	log "[$tag] handoff: $(journalctl -k --since "@$((WAKE_WALL - 400))" --no-pager | grep -E 'handoff' | tail -1 | sed 's/.*zrn-system-sleep: //')"
}
gps_ttff() {	# gps_ttff TAG SECONDS: a reader now; its first fix
	local tag=$1 d=${2:-45} t0
	t0=$(bt)
	python3 $G/nmea-reader.py $d > $H/reader-$tag.out 2>&1
	log "[$tag] GPS reader started +$(d $t0 ${WAKE_BT:-$t0})s after wake: $(grep -E 'connected|first fix|fix \(gap|gate closed|PZTFX' $H/reader-$tag.out | sed 's/ m; HDOP.*//' | tr '\n' '|')"
}
sensors_check() {	# sensors_check TAG: screen on, light and motion stream after a wake
	local tag=$1 bl0 bl1 a1 a2 p1
	bl0=$(cat /sys/class/backlight/*/brightness)
	shell 'screen on'; sleep 4
	bl1=$(cat /sys/class/backlight/*/brightness)
	sctl "watch accel 200"; sctl "watch prox 200"; sleep 2
	a1=$(cat /run/ztoscsf-sensors/accel 2>/dev/null); p1=$(stat -c %Y /run/ztoscsf-sensors/prox 2>/dev/null)
	sleep 1.5
	a2=$(cat /run/ztoscsf-sensors/accel 2>/dev/null)
	log "[$tag] sensors: screen=$(grep screen= /run/ztoscsf-sensors/status) backlight $bl0->$bl1 shell=$(cat /run/user/1000/ztmetro.brightness) accel '$a1' then '$a2' (mtime age $(( $(date +%s) - $(stat -c %Y /run/ztoscsf-sensors/accel 2>/dev/null || echo 0) ))s) prox mtime age $(( $(date +%s) - ${p1:-0} ))s light='$(cat /run/ztoscsf-sensors/light)' light age $(( $(date +%s) - $(stat -c %Y /run/ztoscsf-sensors/light) ))s sensord_journal: $(journalctl -u ztoscsf-sensord --since "@$((WAKE_WALL - 5))" --no-pager -o cat | tail -3 | tr '\n' '|')"
}

mkdir -p $H; R=$H/e2e-${PHASES// /}.out; : > $R; rm -f $H/done
START=$(date +%s)
log "start: $(pids) pdn_applied=$(cat /sys/module/zrn_sleep_init/parameters/pdn_applied) clear_grp4=$(cat /sys/module/zrn_sleep_init/parameters/clear_grp4) inhibit=$(inh) keep=[$(ls /run/ztos-gnss/keep-* /run/user/*/ztos-waydroid-keep-screen 2>/dev/null | tr '\n' ' ')]"
gnss_off || log "GNSS still running at start"

for P in $PHASES; do
T0=$(date +%s)
case $P in
A)	# GPS off through a 60 s sleep; sensors, then GPS within 10 s of the wake
	shell 'screen off'; sleep 6
	sleep_once 60 A
	gps_ttff A 40 &
	after_wake A
	wait
	sensors_check A
	shell 'screen off'
	;;
B)	# a reader through a sleep: must cancel (timed-gnss); then the reader leaves
	shell 'screen on'; sleep 2
	python3 $G/nmea-reader.py 50 > $H/reader-B.out 2>&1 &
	RP=$!
	sleep 15
	shell 'screen off'; sleep 5
	log "[B] reader held, screen off: gnss=$(systemctl is-active ztos-gnss) inhibit=$(inh) mtime-now=$(( $(stat -c %Y $INH/timed-gnss 2>/dev/null || echo 0) - $(date +%s) ))s; sleepd: $(/usr/local/sbin/zrn-sleepd status | head -2 | tr '\n' ' ')"
	sleep_once 60 B
	after_wake B
	wait $RP
	TL=$(bt)
	for i in $(seq 40); do systemctl is-active -q ztos-gnss || break; sleep 0.5; done
	TS=$(bt)
	for i in $(seq 40); do [ -e $INH/timed-gnss ] || break; sleep 0.5; done
	log "[B] reader left: engine $(systemctl is-active ztos-gnss) +$(d $TS $TL)s, inhibit gone +$(d $(bt) $TL)s $(inh); reader: $(grep -E 'first fix|gate closed' $H/reader-B.out | tr '\n' '|')"
	;;
C)	# a reader leaves with the screen on (120 s idle hold), then a sleep at once
	shell 'screen on'; sleep 2
	python3 $G/nmea-reader.py 25 > $H/reader-C.out 2>&1
	log "[C] reader done (screen on): gnss=$(systemctl is-active ztos-gnss) inhibit=$(inh) reader: $(grep -E 'first fix' $H/reader-C.out | tr '\n' '|')"
	sleep_once 60 C
	log "[C] gate: $(journalctl -u ztos-gnss-gate --since "@$T0" --no-pager -o cat | tail -4 | tr '\n' '|')"
	after_wake C
	if [ "$(tail -1 /var/lib/zrn-sleep/log | grep -c cancelled)" = 1 ]; then
		gnss_off; sleep 3; sleep_once 60 C2; after_wake C2; gps_ttff C2 40
	else
		gps_ttff C 40
	fi
	;;
D)	# a reader connects while the sleep is at 'settle': must cancel
	shell 'screen off'; sleep 6
	gnss_off
	( for i in $(seq 400); do
		grep -q settle /run/zrn-sleepd/step 2>/dev/null && break; sleep 0.05
	  done
	  log "[D] step=$(cat /run/zrn-sleepd/step) at reader connect"
	  python3 $G/nmea-reader.py 30 > $H/reader-D.out 2>&1
	  log "[D] reader: $(grep -E 'first fix|gate closed' $H/reader-D.out | tr '\n' '|')" ) &
	sleep_once 60 D
	log "[D] gate: $(journalctl -u ztos-gnss-gate --since "@$T0" --no-pager -o cat | tail -3 | tr '\n' '|') step-now=$(cat /run/zrn-sleepd/step)"
	after_wake D
	wait
	gnss_off
	;;
E)	# a 300 s sleep, GPS off; GPS within 10 s of the wake
	shell 'screen off'; sleep 6
	gnss_off
	sleep_once 300 E
	gps_ttff E 40 &
	after_wake E
	wait
	sensors_check E
	shell 'screen off'
	;;
F)	# kill -9 the gate while it holds the inhibit
	shell 'screen off'; sleep 3
	python3 $G/nmea-reader.py 120 > $H/reader-F.out 2>&1 &
	for i in $(seq 60); do [ -e $INH/timed-gnss ] && break; sleep 0.5; done
	sleep 10
	GP=$(systemctl show -p MainPID --value ztos-gnss-gate)
	log "[F] before kill: gate=$GP gnss=$(systemctl is-active ztos-gnss) inhibit=$(inh) expires in $(( $(stat -c %Y $INH/timed-gnss 2>/dev/null || echo 0) - $(date +%s) ))s"
	kill -9 $GP; TK=$(bt)
	gone=""; stopped=""
	for i in $(seq 200); do
		[ -z "$stopped" ] && ! systemctl is-active -q ztos-gnss && stopped=$(d $(bt) $TK)
		[ -z "$gone" ] && [ ! -e $INH/timed-gnss ] && gone=$(d $(bt) $TK)
		[ -n "$gone" ] && [ -n "$stopped" ] && break
		sleep 0.5
	done
	log "[F] after kill: inhibit gone +${gone:-never}s, engine stopped +${stopped:-never}s, new gate=$(systemctl show -p MainPID --value ztos-gnss-gate) $(systemctl is-active ztos-gnss-gate); reader: $(grep -E 'first fix|gate closed' $H/reader-F.out | tr '\n' '|')"
	kill $! 2>/dev/null; wait
	gnss_off
	;;
G)	# J2 cap: a reader held over 10 min with the screen off; then the screen on
	shell 'screen off'; sleep 3
	gnss_off
	python3 $G/nmea-reader.py 760 > $H/reader-G.out 2>&1 &
	RP=$!; TR=$(bt)
	for i in $(seq 700); do
		journalctl -u ztos-gnss-gate --since "@$T0" --no-pager -o cat | grep -q 'screen-off location cap:' && break; sleep 1
	done
	TC=$(bt)
	for i in $(seq 20); do systemctl is-active -q ztos-gnss || break; sleep 0.5; done
	log "[G] cap +$(d $TC $TR)s after the reader; engine $(systemctl is-active ztos-gnss) +$(d $(bt) $TC)s; inhibit=$(inh)"
	sleep 20
	log "[G] capped, screen off: gnss=$(systemctl is-active ztos-gnss) inhibit=$(inh) gate: $(journalctl -u ztos-gnss-gate --since "@$T0" --no-pager -o cat | tail -3 | tr '\n' '|')"
	shell 'screen on'; TO=$(bt)
	for i in $(seq 60); do systemctl is-active -q ztos-gnss && break; sleep 0.5; done
	log "[G] screen on: engine $(systemctl is-active ztos-gnss) +$(d $(bt) $TO)s inhibit=$(inh)"
	wait $RP
	log "[G] reader: $(grep -E 'connected|first fix|fix \(gap|gate closed' $H/reader-G.out | tr '\n' '|') gate: $(journalctl -u ztos-gnss-gate --since "@$T0" --no-pager -o cat | grep -E 'cap|screen on|GPS on' | tr '\n' '|')"
	shell 'screen off'
	gnss_off
	;;
esac
log "[$P] crash lines in phase: $(crash $T0 | wc -l) $(crash $T0 | sed 's/^\[ *\([0-9.]*\)\]/\1/' | cut -c1-140 | tr '\n' '|')"
done
shell 'screen off'
log "end: $(pids) sleep log: $(tail -n 3 /var/lib/zrn-sleep/log | cut -c12- | tr '\n' '|')"
touch $H/done
