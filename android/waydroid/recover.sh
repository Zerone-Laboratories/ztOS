#!/usr/bin/env bash
# Root-only helper. Package names and APK paths are validated per action.
set -euo pipefail
[[ $EUID == 0 && $# -ge 1 ]] || exit 2
. /usr/lib/ztos/user.sh
lxc_args=(-P /var/lib/waydroid/lxc -n waydroid)
# Frozen, or stuck half way (FREEZING: something thawed the cgroup under a
# freeze LXC was still waiting on): running again either way.
thaw() {
    case $(lxc-info "${lxc_args[@]}" -sH 2>/dev/null) in
        FROZEN|FREEZING)
            timeout 10 waydroid container unfreeze || true
            [[ $(lxc-info "${lxc_args[@]}" -sH 2>/dev/null) == RUNNING ]] || lxc-unfreeze "${lxc_args[@]}" || true
            ;;
    esac
}
case $1 in
    audio)
        [[ $# == 1 ]] || exit 2
        /usr/local/libexec/ztos-waydroid-audio
        ;;
    prepare)
        [[ $# == 1 ]] || exit 2
        systemctl start waydroid-container.service
        thaw
        /usr/local/libexec/ztos-waydroid-audio
        ;;
    freeze)
        [[ $# == 1 ]] || exit 2
        if [[ $(lxc-info "${lxc_args[@]}" -sH 2>/dev/null) == RUNNING ]]; then
            timeout 5 waydroid container freeze
        fi
        ;;
    unfreeze)
        [[ $# == 1 ]] || exit 2
        systemctl start waydroid-container.service
        thaw
        /usr/local/libexec/ztos-waydroid-audio
        ;;
    install)
        [[ $# == 2 ]] || exit 2
        apk=$(realpath -e -- "$2") || exit 2
        [[ $apk == "$ZTOS_HOME"/.local/share/ztos/android/apks/*.apk && -f $apk ]] || exit 2
        ztos_as_user WAYLAND_DISPLAY=wayland-0 \
            timeout 90 waydroid app install "$apk"
        ;;
    remove)
        [[ $# == 2 && $2 =~ ^[a-zA-Z][a-zA-Z0-9_]*(\.[a-zA-Z0-9_]+)+$ ]] || exit 2
        ztos_as_user WAYLAND_DISPLAY=wayland-0 \
            timeout 60 waydroid app remove "$2"
        ;;
    close)
        [[ $# == 2 && $2 =~ ^[a-zA-Z][a-zA-Z0-9_]*(\.[a-zA-Z0-9_]+)+$ ]] || exit 2
        env XDG_RUNTIME_DIR="$ZTOS_RUNTIME_DIR" DBUS_SESSION_BUS_ADDRESS="unix:path=$ZTOS_RUNTIME_DIR/bus" WAYLAND_DISPLAY=wayland-0 \
            timeout 10 waydroid shell -- am force-stop "$2"
        ;;
    recover)
        [[ $# == 1 ]] || exit 2
        systemctl restart waydroid-container.service
        ;;
    # zrn-pushd: one push message to an app, the way microG's McsService
    # broadcasts it (RECEIVE to the package, waking a stopped one, with a
    # temporary power allowlist). The extras come on stdin as records of
    # TYPE NUL KEY NUL VALUE NUL, TYPE s (string), l (long) or i (int).
    # Exit 4: the package has no RECEIVE receiver; 3: Android is not up.
    push)
        [[ $# == 2 && $2 =~ ^[a-zA-Z][a-zA-Z0-9_]*(\.[a-zA-Z0-9_]+)+$ ]] || exit 2
        thaw
        [[ $(lxc-info "${lxc_args[@]}" -sH 2>/dev/null) == RUNNING ]] || exit 3
        extras=()
        while IFS= read -r -d '' type && IFS= read -r -d '' key && IFS= read -r -d '' value; do
            (( ${#extras[@]} < 600 )) || exit 2
            case $type in
                s) extras+=(--es "$key" "$value") ;;
                l) [[ $value =~ ^-?[0-9]{1,19}$ ]] || exit 2; extras+=(--el "$key" "$value") ;;
                i) [[ $value =~ ^-?[0-9]{1,10}$ ]] || exit 2; extras+=(--ei "$key" "$value") ;;
                *) exit 2 ;;
            esac
        done
        # One shell for the three steps: each `waydroid shell` costs ~2 s.
        # 0x10000020: FLAG_RECEIVER_FOREGROUND | FLAG_INCLUDE_STOPPED_PACKAGES
        # (ztOS closes an app with force-stop; it must still get its messages).
        # `waydroid shell` does not pass the exit status on: the script says it.
        out=$(timeout 60 waydroid shell -- /system/bin/sh -c '
            p=$1; shift
            [ "$(getprop sys.boot_completed)" = 1 ] || { echo zrn-result=3; exit; }
            case $(cmd package query-receivers --brief -a com.google.android.c2dm.intent.RECEIVE -p "$p" 2>&1) in
                *"$p/"*) ;;
                *) echo "no receiver for $p"; echo zrn-result=4; exit ;;
            esac
            cmd deviceidle tempwhitelist -d 60000 "$p" >/dev/null 2>&1
            out=$(am broadcast --user 0 -a com.google.android.c2dm.intent.RECEIVE -p "$p" -f 0x10000020 "$@" 2>&1)
            case $out in
                *"Broadcast completed"*) echo zrn-result=0 ;;
                *) echo "$out" | tail -n 2; echo zrn-result=5 ;;
            esac' sh "$2" "${extras[@]}" 2>&1) || true
        rc=${out##*zrn-result=}
        [[ $rc =~ ^[0-9]$ ]] || { printf '%s\n' "$out" | tail -n 3 >&2; exit 5; }
        [[ $rc == 0 ]] || printf '%s\n' "${out%zrn-result=*}" | grep -v '^$' | tail -n 3 >&2
        exit "$rc"
        ;;
    # zrn-pushd: microG's device identity (androidId, securityToken from its
    # check-in), and microG's own MCS connection kept off so that only
    # zrn-pushd holds the device's push connection. gcm_network_* = -1 is
    # microG's "off" for a network type (GcmPrefs.isEnabledFor: a negative
    # heartbeat); registration needs only gcm_enable_mcs_service.
    microg)
        [[ $# == 1 ]] || exit 2
        thaw
        [[ $(lxc-info "${lxc_args[@]}" -sH 2>/dev/null) == RUNNING ]] || exit 3
        # microG's process may have to start for its provider first (the
        # process limit is 0): the first call can fail, so three tries
        for try in 1 2 3; do
            out=$(timeout 60 waydroid shell -- /system/bin/sh -c '
                u=content://com.google.android.gms.microg.settings
                content update --uri $u/gcm --bind gcm_enable_mcs_service:b:true --bind gcm_confirm_new_apps:b:false \
                    --bind gcm_network_mobile:i:-1 --bind gcm_network_wifi:i:-1 \
                    --bind gcm_network_roaming:i:-1 --bind gcm_network_other:i:-1 >/dev/null 2>&1 || exit
                row=$(content query --uri $u/check-in --projection androidId:securityToken 2>/dev/null) || exit
                a=${row#*androidId=}; a=${a%%,*}; a=${a%% *}
                t=${row#*securityToken=}; t=${t%%,*}; t=${t%% *}
                echo "android_id=$a"; echo "security_token=$t"' 2>/dev/null) || true
            a=$(sed -n 's/^android_id=\(-\?[0-9]\{1,20\}\)$/\1/p' <<< "$out")
            t=$(sed -n 's/^security_token=\(-\?[0-9]\{1,20\}\)$/\1/p' <<< "$out")
            [[ -n $a && -n $t ]] && break
            sleep 3
        done
        [[ -n $a && -n $t ]] || exit 6
        printf 'android_id=%s\nsecurity_token=%s\n' "$a" "$t"
        ;;
    ready)
        [[ $# == 1 ]] || exit 2
        deadline=$((SECONDS + 90))
        while (( SECONDS < deadline )); do
            thaw
            if [[ $(lxc-info "${lxc_args[@]}" -sH 2>/dev/null) == RUNNING ]] &&
               [[ $(timeout 3 lxc-attach "${lxc_args[@]}" --clear-env -v PATH=/system/bin:/system/xbin:/vendor/bin -- /system/bin/getprop sys.boot_completed 2>/dev/null) == 1 ]] &&
               timeout 3 lxc-attach "${lxc_args[@]}" --clear-env -v PATH=/system/bin:/system/xbin:/vendor/bin -- /system/bin/service check package 2>/dev/null | grep -q ': found$' &&
               timeout 3 lxc-attach "${lxc_args[@]}" -- /system/bin/service check waydroidplatform 2>/dev/null | grep -q ': found$'; then
                /usr/local/libexec/ztos-waydroid-audio
                exit 0
            fi
            sleep 1
        done
        echo 'Android failed readiness check.' >&2
        exit 1
        ;;
    *) exit 2 ;;
esac
