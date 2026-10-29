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
    settings
}
# A command in Android, at once (lxc-attach: `waydroid shell` costs ~2 s). It
# reads nothing: push's records on stdin are not its.
android_sh() {
    timeout 10 lxc-attach "${lxc_args[@]}" --clear-env -v PATH=/system/bin:/system/xbin:/vendor/bin -- /system/bin/sh -c "$@" < /dev/null
}
# Android's settings as ztOS has them.
# - Sounds: ztOS chimes for every notification itself, at its ringer, so
#   Android's notification stream is kept at 0 (it is not linked to the ring
#   stream here: volume_separate_notification). Calls ring through Android's
#   ring stream, which follows the ztOS ringer, 0 at vibrate. Android's own
#   touch, lock and charging sounds are off.
# - Waydroid's keyboard gets a layout: without one, every time Android's
#   display sleeps (display off), Android posts "Configure physical keyboard"
#   again, and ztOS chimes for it. There is no shell command for it in
#   Android 13: IInputManager's setCurrentKeyboardLayoutForInputDevice is
#   transaction 18 there (17 reads it), InputDeviceIdentifier written as
#   descriptor, vendor, product.
# What was given is kept by Android across boots; it is given again when the
# ringer changes or Android has started again since (its init's pid is in the
# stamp). A frozen Android is left as it is: thaw() calls this again.
settings() {
    [[ $(lxc-info "${lxc_args[@]}" -sH 2>/dev/null) == RUNNING ]] || return 0
    local stamp=/run/ztos-waydroid-settings percent ring want
    exec 9>"$stamp.lock"
    flock -w 15 9 || return 0
    percent=$(sed -n 's/^[[:space:]]*ring_volume[[:space:]]*=[[:space:]]*\([0-9]\{1,3\}\)[[:space:]]*$/\1/p' \
        "$ZTOS_HOME/.config/ztmetro/config" 2>/dev/null | tail -n 1) || true
    [[ -n $percent ]] || percent=70	# ztmetro's own default, 21 of 30
    percent=$((10#$percent))
    (( percent <= 100 )) || percent=100
    ring=$(( (percent * 7 + 50) / 100 ))
    (( percent == 0 || ring > 0 )) || ring=1
    want="$(lxc-info "${lxc_args[@]}" -pH 2>/dev/null) $ring"
    if [[ $(cat "$stamp" 2>/dev/null) != "$want" ]] &&
       [[ $(android_sh '
            [ "$(getprop sys.boot_completed)" = 1 ] || exit
            [ "$(cmd device_config get systemui volume_separate_notification)" = true ] ||
                cmd device_config put systemui volume_separate_notification true
            # the ring stream first: leaving vibrate unmutes notifications at 1
            cmd media_session volume --stream 2 --set "$1" || exit
            cmd media_session volume --stream 5 --set 0 || exit
            settings put system sound_effects_enabled 0
            settings put system lockscreen_sounds_enabled 0
            settings put system dtmf_tone 0
            settings put global charging_sounds_enabled 0
            settings put global power_sounds_enabled 0
            if [ "$(getprop ro.build.version.sdk)" = 33 ]; then
                k=$(dumpsys input | grep -A12 ": wayland_keyboard$")
                d=$(echo "$k" | sed -n "s/^ *Descriptor: \([0-9a-f]*\)$/\1/p" | head -n 1)
                v=$(echo "$k" | sed -n "s/.*vendor=0x\([0-9a-f]*\),.*/\1/p" | head -n 1)
                r=$(echo "$k" | sed -n "s/.*product=0x\([0-9a-f]*\),.*/\1/p" | head -n 1)
                if [ -n "$d" ] && [ -n "$v" ] && [ -n "$r" ]; then
                    id="i32 1 s16 $d i32 $((0x$v)) i32 $((0x$r))"
                    case $(service call input 17 $id) in
                        *ffffffff*) service call input 18 $id s16 \
                            com.android.inputdevices/com.android.inputdevices.InputDeviceReceiver/keyboard_layout_english_us >/dev/null ;;
                    esac
                fi
            fi
            echo zrn-settings=ok' sh "$ring" 2>/dev/null) == *zrn-settings=ok* ]]; then
        printf '%s\n' "$want" > "$stamp"
    fi
    exec 9>&-
}
display() {
    [[ $(lxc-info "${lxc_args[@]}" -sH 2>/dev/null) == RUNNING ]] || return 0
    local key=KEYCODE_SLEEP
    [[ $1 == on ]] && key=KEYCODE_WAKEUP
    android_sh '[ "$(getprop sys.boot_completed)" = 1 ] && cmd input keyevent '"$key" >/dev/null 2>&1 || true
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
        display on
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
    # Android's display is on only while one of its windows is in front in
    # ztOS (ztos-waydroid-power). Asleep, its activities are stopped, as on a
    # phone with the screen off: a chat left open on Home no longer reads the
    # messages that come in. unfreeze leaves it asleep, so that zrn-pushd's
    # thaw for a push message does not bring the app to the front again.
    display)
        [[ $# == 2 && ($2 == on || $2 == off) ]] || exit 2
        display "$2"
        ;;
    settings)
        [[ $# == 1 ]] || exit 2
        settings
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
        # The app fetches what the message announced: a phone that sleeps
        # (zrn-sleepd's timed inhibit, where there is one) waits a minute.
        [[ -d /run/zrn-sleepd/inhibit ]] &&
            touch -d "@$(( $(date +%s) + 60 ))" /run/zrn-sleepd/inhibit/timed-push 2>/dev/null
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
                settings
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
