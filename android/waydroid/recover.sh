#!/usr/bin/env bash
# Root-only helper. Package names and APK paths are validated per action.
set -euo pipefail
[[ $EUID == 0 && $# -ge 1 ]] || exit 2
. /usr/lib/ztos/user.sh
lxc_args=(-P /var/lib/waydroid/lxc -n waydroid)
case $1 in
    audio)
        [[ $# == 1 ]] || exit 2
        /usr/local/libexec/ztos-waydroid-audio
        ;;
    prepare)
        [[ $# == 1 ]] || exit 2
        systemctl start waydroid-container.service
        if [[ $(lxc-info "${lxc_args[@]}" -sH 2>/dev/null) == FROZEN ]]; then
            timeout 10 waydroid container unfreeze
        fi
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
        if [[ $(lxc-info "${lxc_args[@]}" -sH 2>/dev/null) == FROZEN ]]; then
            timeout 10 waydroid container unfreeze
        fi
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
    ready)
        [[ $# == 1 ]] || exit 2
        deadline=$((SECONDS + 90))
        while (( SECONDS < deadline )); do
            if [[ $(lxc-info "${lxc_args[@]}" -sH 2>/dev/null) == FROZEN ]]; then
                timeout 5 waydroid container unfreeze || true
            fi
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
