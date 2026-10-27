#!/usr/bin/env bash
# Run after Android boots. Preserve the previous WhatsApp battery exemption.
set -euo pipefail
[[ $EUID == 0 ]] || { echo 'Run as root.' >&2; exit 1; }
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
helper=${ANDROID_SHELL_HELPER:-$here/microg-android-shell.sh}
backup=/var/lib/waydroid/ztos-backup/oldsdk
mkdir -p "$backup"
[[ -f $helper ]] || { echo "Missing Android shell helper: $helper" >&2; exit 1; }
state=$backup/whatsapp-deviceidle.before
if [[ ${1:-} == --rollback ]]; then
    if [[ -f $state && $(cat "$state") == absent ]]; then
        bash "$helper" -c 'cmd deviceidle whitelist -com.whatsapp'
    fi
else
    current=$(bash "$helper" -c 'cmd deviceidle whitelist')
    if [[ ! -f $state ]]; then
        if grep -q '^user,com.whatsapp,' <<<"$current"; then
            echo present > "$state"
        else
            echo absent > "$state"
        fi
    fi
    bash "$helper" -c 'cmd deviceidle whitelist +com.whatsapp'
fi
bash "$helper" -c 'cmd deviceidle whitelist | grep com.whatsapp || true'
