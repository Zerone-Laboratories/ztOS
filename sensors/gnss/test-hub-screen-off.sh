#!/bin/bash
# Which screen-off asserts the S9 hub's firmware (transport_layer.c line 165)?
# No SoC sleep here. Each case: screen on, maybe `watch` a sensor through
# ztoscsf-sensord, screen off, then 40 s of journal. Root, via systemd-run:
#   systemd-run --unit=zrn-iso --collect -E CASES="plain accel prox both" test-hub-screen-off.sh
# 30 Oct 2026: plain 0 asserts (2 runs); accel, prox, both: an assert and a
# hub crash every time (8 of 8), Arg2=5, also with the sensor's delay in the
# REMOVE payload. The hub recovers by itself (ztoscsf power retry).
H=${OUT:-/home/zerone/team-test}; mkdir -p $H; R=$H/iso.out; CTL=/run/user/1000/ztmetro.ctl; SCTL=/run/ztoscsf-sensord.ctl
log() { echo "$(date +%T.%3N) $*" >> $R; }
shell() { timeout 3 sh -c "echo \"$1\" > $CTL"; }
sctl() { timeout 3 sh -c "echo \"$1\" > $SCTL"; }
waitready() { for i in $(seq 90); do if [ "$(cat /sys/class/sec/sensorhub/ready)" = 1 ] && pgrep -x lhd >/dev/null && ! journalctl --since -8s --no-pager -o cat | grep -q 'ESW:CRASHED\|power retry'; then return; fi; sleep 1; done; }
: > $R
for c in ${CASES:-plain accel prox both}; do
	waitready; sleep 10
	T=$(date +%s)
	shell 'screen on'; sleep 6
	case $c in accel) sctl 'watch accel 200';; prox) sctl 'watch prox 200';; both) sctl 'watch accel 200'; sctl 'watch prox 200';; esac
	sleep 3
	shell 'screen off'
	sleep 40
	log "[$c] asserts=$(journalctl --since @$T --no-pager -o cat | grep -c 'line=0165') crashed=$(journalctl --since @$T --no-pager -o cat | grep -c 'zrn-bbd: control: ESW:CRASHED') lhd=$(pgrep -x lhd) $(journalctl --since @$T --no-pager -o cat | grep -oE 'Arg2=[0-9A-F]+' | sort -u | tr '\n' ' ')"
done
touch $H/iso.done
