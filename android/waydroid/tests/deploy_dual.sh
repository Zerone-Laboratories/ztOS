#!/usr/bin/env bash
set -euo pipefail
. /usr/lib/ztos/user.sh
work=$ZTOS_HOME/ztmetro-phase2
mode=${1:?helpers or shell}
backup_install() {
    local source=$1 target=$2
    [[ -e $target.whatsapp-dual-backup ]] || cp -a "$target" "$target.whatsapp-dual-backup"
    install -m755 "$source" "$target.dual-new"
    mv "$target.dual-new" "$target"
}
if [[ $mode == helpers ]]; then
    backup_install "$work/power.dual.sh" /usr/local/bin/ztos-waydroid-power
    backup_install "$work/network.dual.py" /usr/local/libexec/ztos-waydroid-network
    backup_install "$work/ztos-android.dual" /usr/local/bin/ztos-android
    systemctl restart ztos-waydroid-network.service
elif [[ $mode == shell ]]; then
    backup_install "$work/ztmetro" /usr/local/bin/ztmetro
    pkill -x ztmetro || true
    sleep 8
    if ! pgrep -x ztmetro >/dev/null || ! grep -q 'ztMETRO on WAYLAND_DISPLAY' "$ZTOS_HOME/.local/state/ztmetro.log"; then
        install -m755 /usr/local/bin/ztmetro.whatsapp-dual-backup /usr/local/bin/ztmetro.rollback
        mv /usr/local/bin/ztmetro.rollback /usr/local/bin/ztmetro
        pkill -x ztmetro || true
        echo 'Shell restart failed; restored pre-dual binary.' >&2
        exit 1
    fi
    pgrep -a ztmetro
    sha256sum "$work/ztmetro" /usr/local/bin/ztmetro
else
    exit 2
fi
