#!/bin/sh
# Detached battery-only comparison; automatic hardware wake after each trial.
# Install beside suspend-test.py and firmware-state.py, then run through systemd.
set -eu
. /usr/lib/ztos/user.sh
SELF=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LOG=${1:-/var/lib/ztos-power-test/$(date -u +%Y%m%d)}
mkdir -p "$LOG"
# The display may have been re-lit while the detached service waited for the
# cable to be removed. Ask zrnperfd to blank it now, then require DRM to
# confirm the panel is off before the suspend helper starts.
CTL=$ZTOS_RUNTIME_DIR/ztmetro.ctl
if [ -p "$CTL" ]; then
    printf 'screen off\n' > "$CTL"
fi
i=0
while :; do
    off=1
    found=0
    for output in /sys/class/drm/card*-*/enabled; do
        [ -f "$output" ] || continue
        found=1
        [ "$(cat "$output")" = disabled ] || off=0
    done
    [ "$found" -eq 1 ] && [ "$off" -eq 1 ] && break
    i=$((i + 1))
    if [ "$i" -ge 75 ]; then
        echo '{"phase":"display_off_timeout"}' > "$LOG/display-off-timeout.json"
        exit 1
    fi
    sleep 0.2
done
python3 -u "$SELF/suspend-test.py" --wait-unplug 600 --battery-only \
    --seconds 20 > "$LOG/battery-suspend-baseline.log" 2>&1
python3 -u "$SELF/suspend-test.py" --battery-only --cpd --sicd \
    --seconds 30 > "$LOG/battery-suspend-sicd.log" 2>&1
python3 -u "$SELF/suspend-test.py" --battery-only --cpd --sicd --sicd-cpu 0 \
    --allow-mif-sleep --seconds 30 > "$LOG/battery-suspend-sicd-cpu0.log" 2>&1
