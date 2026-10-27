#!/usr/bin/env bash
# Run as root through the approved phone controller after the native build.
set -euo pipefail
. /usr/lib/ztos/user.sh
work=$ZTOS_HOME/ztmetro-phase2
files=(ztmetro ztos-waydroid-launch ztos-waydroid-power)
for file in "${files[@]}"; do
    [[ -f /usr/local/bin/$file.reliability-policy-backup ]] ||
        cp -a "/usr/local/bin/$file" "/usr/local/bin/$file.reliability-policy-backup"
done
rollback() {
    trap - ERR
    for file in "${files[@]}"; do
        install -m755 "/usr/local/bin/$file.reliability-policy-backup" "/usr/local/bin/$file.rollback"
        mv "/usr/local/bin/$file.rollback" "/usr/local/bin/$file"
    done
    pkill -x ztmetro || true
    sleep 8
    echo 'Reliability deployment failed; restored all three backups.' >&2
    exit 1
}
trap rollback ERR
install -m755 "$work/ztmetro" /usr/local/bin/ztmetro.reliability-new
install -m755 "$work/reliability-launch.sh" /usr/local/bin/ztos-waydroid-launch.reliability-new
install -m755 "$work/reliability-power.sh" /usr/local/bin/ztos-waydroid-power.reliability-new
for file in "${files[@]}"; do
    mv "/usr/local/bin/$file.reliability-new" "/usr/local/bin/$file"
done
pkill -x ztmetro || true
sleep 8
pgrep -x ztmetro
grep -q 'ztMETRO on WAYLAND_DISPLAY' "$ZTOS_HOME/.local/state/ztmetro.log"
grep -q 'Panfrost' "$ZTOS_HOME/.local/state/ztmetro.log"
cmp "$work/ztmetro" /usr/local/bin/ztmetro
trap - ERR
sha256sum /usr/local/bin/ztmetro /usr/local/bin/ztos-waydroid-launch /usr/local/bin/ztos-waydroid-power
