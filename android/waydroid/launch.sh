#!/usr/bin/env bash
# Public Phase 1 launch entry point; shell integration belongs to Phase 2.
set -euo pipefail
PS4='+ ${SECONDS}s '
set -x
install_apk=
remove_package=
if [[ ${1:-} == --install && $# == 2 && -f $2 ]]; then
    install_apk=$(realpath -- "$2")
elif [[ ${1:-} == --remove && $# == 2 && $2 =~ ^[a-zA-Z][a-zA-Z0-9_]*(\.[a-zA-Z0-9_]+)+$ ]]; then
    remove_package=$2
elif [[ $# == 1 && $1 =~ ^[a-zA-Z][a-zA-Z0-9_]*(\.[a-zA-Z0-9_]+)+$ ]]; then
    package=$1
else
    echo 'Usage: ztos-waydroid-launch PACKAGE | --install APK' >&2; exit 2
fi
if [[ -n $install_apk ]]; then
    package=$(ztos-android inspect "$install_apk" | cut -f1)
fi
[[ -z $remove_package ]] || package=$remove_package
[[ $package =~ ^[a-zA-Z][a-zA-Z0-9_]*(\.[a-zA-Z0-9_]+)+$ ]] || {
    echo 'APK has no valid package name.' >&2; exit 2;
}
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0}
export DBUS_SESSION_BUS_ADDRESS=${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}
exec 9>"$XDG_RUNTIME_DIR/ztos-waydroid-launch.lock"
flock -w 240 9
launch_marker="$XDG_RUNTIME_DIR/ztos-waydroid-launching"
printf '%s\n' "$$" > "$launch_marker"
trap 'rm -f "$launch_marker"; ztos-waydroid-power sync || true' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
systemctl --user import-environment WAYLAND_DISPLAY XDG_RUNTIME_DIR
# Android's composer keeps its old Wayland connection after ztMETRO exits.
# DBus and Android readiness can still pass; replace that stale session before
# issuing the first app launch against a new compositor socket.
compositor_socket=$WAYLAND_DISPLAY
[[ $compositor_socket == /* ]] || compositor_socket="$XDG_RUNTIME_DIR/$compositor_socket"
compositor_generation=$(stat -Lc '%d:%i:%z' -- "$compositor_socket")
session_generation="$XDG_RUNTIME_DIR/ztos-waydroid-compositor"
if [[ -r $session_generation ]] &&
   [[ $(cat "$session_generation") != "$compositor_generation" ]]; then
    echo 'Wayland compositor changed; replacing stale Android session before launch.' >&2
    timeout 25 waydroid session stop || true
    systemctl --user stop ztos-waydroid-session.service || true
    sudo -n /usr/local/libexec/ztos-waydroid-recover recover
fi
for attempt in 1 2; do
    rm -f "$XDG_RUNTIME_DIR/ztos-waydroid-visible"
    sudo -n /usr/local/libexec/ztos-waydroid-recover prepare
    systemctl --user reset-failed ztos-waydroid-session.service 2>/dev/null || true
    systemctl --user start ztos-waydroid-session.service
    responsive=false
    for ((i=0; i<5; i++)); do
        if busctl --user --timeout=2 call id.waydro.Session /SessionManager org.freedesktop.DBus.Peer Ping >/dev/null 2>&1; then
            responsive=true
            break
        fi
        sleep 1
    done
    if "$responsive" && sudo -n /usr/local/libexec/ztos-waydroid-recover ready &&
       { if [[ -n $install_apk ]]; then sudo -n /usr/local/libexec/ztos-waydroid-recover install "$install_apk"; elif [[ -n $remove_package ]]; then sudo -n /usr/local/libexec/ztos-waydroid-recover remove "$remove_package"; else timeout 20 waydroid app launch "$package"; fi; }; then
        printf '%s\n' "$compositor_generation" > "$session_generation"
        if [[ -n $install_apk || -n $remove_package ]]; then
            for ((i=0; i<3; i++)); do
                if apps=$(timeout 5 waydroid app list) && grep -q "^packageName: " <<< "$apps"; then
                    if [[ -n $install_apk ]] && grep -Fqx "packageName: $package" <<< "$apps"; then exit 0; fi
                    if [[ -n $remove_package ]] && ! grep -Fqx "packageName: $package" <<< "$apps"; then exit 0; fi
                fi
                sleep 1
            done
            echo "Waydroid did not confirm package operation for $package." >&2
        else
            # A different app or the boot surface can already count as a window.
            # Keep the launch marker until ztMETRO has shown this package.
            for ((i=0; i<20; i++)); do
                if [[ -p $XDG_RUNTIME_DIR/ztmetro.ctl ]]; then
                    timeout 1 bash -c 'printf "focus waydroid.%s\n" "$1" > "$2"' _ \
                        "$package" "$XDG_RUNTIME_DIR/ztmetro.ctl" || true
                    if [[ -r $XDG_RUNTIME_DIR/ztos-waydroid-visible ]] &&
                       [[ $(cat "$XDG_RUNTIME_DIR/ztos-waydroid-visible") == "$package" ]]; then
                        exit 0
                    fi
                elif timeout 3 waydroid prop get waydroid.open_windows | grep -Eq '^[1-9][0-9]*$'; then
                    exit 0
                fi
                sleep 1
            done
            echo "No visible Waydroid window for $package." >&2
        fi
    fi
    if [[ $attempt == 1 ]]; then
        timeout 25 waydroid session stop || true
        systemctl --user stop ztos-waydroid-session.service || true
        sudo -n /usr/local/libexec/ztos-waydroid-recover recover
    fi
done
echo 'Waydroid launch failed after one recovery attempt.' >&2
exit 1
