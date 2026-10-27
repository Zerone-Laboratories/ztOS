#!/usr/bin/env bash
set -euo pipefail
. /usr/lib/ztos/user.sh
work=$ZTOS_HOME/ztmetro-phase2
backup=/usr/local/bin/ztmetro.launch-bug-backup
[[ -f $backup ]] || cp -a /usr/local/bin/ztmetro "$backup"
install -m755 "$work/ztmetro" /usr/local/bin/ztmetro.launch-new
mv /usr/local/bin/ztmetro.launch-new /usr/local/bin/ztmetro
pkill -x ztmetro || true
sleep 8
if ! pgrep -x ztmetro >/dev/null || ! grep -q 'ztMETRO on WAYLAND_DISPLAY' "$ZTOS_HOME/.local/state/ztmetro.log"; then
    install -m755 "$backup" /usr/local/bin/ztmetro.rollback
    mv /usr/local/bin/ztmetro.rollback /usr/local/bin/ztmetro
    pkill -x ztmetro || true
    echo 'Shell verification failed; restored backup.' >&2
    exit 1
fi
pgrep -a ztmetro
