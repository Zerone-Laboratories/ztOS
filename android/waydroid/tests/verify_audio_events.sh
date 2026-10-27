#!/bin/bash
# Run on the phone as root after deploying the recovery hooks and AudioPlaybackProbe.dex.
# Requires /tmp/ztos-audio-amp-status.py and /data/local/tmp/ztos-audio-probe.dex.
# Exercises only audio servers and container freeze/thaw; does not restart Android.
set -euo pipefail
. /usr/lib/ztos/user.sh
export XDG_RUNTIME_DIR=$ZTOS_RUNTIME_DIR
u() { runuser -u "$ZTOS_USER" -- "$@"; }
a() { lxc-attach -P /var/lib/waydroid/lxc -n waydroid -- /system/bin/sh -c "$1"; }
hal() { a '/system/bin/pidof android.hardware.audio.service'; }
probe() {
 timeout 20 waydroid shell -- /system/bin/sh -c 'CLASSPATH=/data/local/tmp/ztos-audio-probe.dex /system/bin/app_process /system/bin AudioPlaybackProbe' > /tmp/audio-event-probe.log 2>&1 &
 probe_pid=$!
 for attempt in {1..20}; do
  if grep -q 'state: RUNNING' /proc/asound/card0/pcm0p/sub0/status; then break; fi
  sleep 0.5
 done
 cat /proc/asound/card0/pcm0p/sub0/status
 grep -q "state: RUNNING" /proc/asound/card0/pcm0p/sub0/status
 u wpctl status
 python /tmp/ztos-audio-amp-status.py
 u timeout 3 pw-top -b -n 2 || true
 wait "$probe_pid" || [[ $? == 124 ]]
 cat /tmp/audio-event-probe.log
 grep -q 'final head=384000' /tmp/audio-event-probe.log
}
echo RUNNING_RESTART
/usr/local/libexec/ztos-waydroid-recover unfreeze
before=$(hal)
u systemctl --user restart pipewire.service pipewire-pulse.service wireplumber.service
sleep 2
after=$(hal)
echo "HAL before=$before after=$after"
[[ $before != "$after" ]]
probe
echo PULSE_ONLY_RESTART
before=$(hal)
u systemctl --user restart pipewire-pulse.service
sleep 2
after=$(hal)
echo "HAL before=$before after=$after"
[[ $before != "$after" ]]
echo FROZEN_RESTART
/usr/local/libexec/ztos-waydroid-recover freeze
before=$(cat /run/ztos-waydroid-audio.generation)
u systemctl --user restart pipewire.service pipewire-pulse.service wireplumber.service
sleep 2
lxc-info -P /var/lib/waydroid/lxc -n waydroid -s
[[ $(lxc-info -P /var/lib/waydroid/lxc -n waydroid -sH) == FROZEN ]]
[[ $before == "$(cat /run/ztos-waydroid-audio.generation)" ]]
echo 'generation unchanged while frozen'
/usr/local/libexec/ztos-waydroid-recover unfreeze
[[ $before != "$(cat /run/ztos-waydroid-audio.generation)" ]]
probe
echo SLEEPD_AUDIO_STOP_START
before=$(hal)
u systemctl --user stop wireplumber.service pipewire.service pipewire.socket
u systemctl --user show ztos-waydroid-audio-reconnect.service -p ActiveState -p MainPID
u systemctl --user start pipewire.socket pipewire.service wireplumber.service
sleep 2
after=$(hal)
echo "HAL before=$before after=$after"
[[ $before != "$after" ]]
probe
echo UNCHANGED_THAW
before=$(hal)
/usr/local/libexec/ztos-waydroid-recover freeze
/usr/local/libexec/ztos-waydroid-recover unfreeze
after=$(hal)
echo "HAL before=$before after=$after"
[[ $before == "$after" ]]
echo IDLE_CHECK
u systemctl --user show ztos-waydroid-audio-reconnect.service -p ActiveState -p SubState -p MainPID -p TasksCurrent -p CPUUsageNSec
for round in 1 2; do
 python - <<'PY'
from pathlib import Path
matches=[]
for p in Path('/proc').glob('[0-9]*/cmdline'):
 try:
  args=p.read_bytes().split(b'\0')
  if any(x in (b'/usr/local/libexec/ztos-waydroid-audio',b'/usr/local/libexec/ztos-waydroid-recover') for x in args): matches.append((p.parent.name,args))
 except OSError: pass
print('recovery processes:', matches)
assert not matches
PY
 sleep 10
done
u systemctl --user show ztos-waydroid-audio-reconnect.service -p CPUUsageNSec
systemctl show ztos-waydroid-audio.service -p LoadState -p ActiveState
u systemctl --user is-enabled ztos-waydroid-audio-reconnect.service
