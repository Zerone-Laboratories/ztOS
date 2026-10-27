#!/bin/sh
# Run on the phone, from ztmetro/ in the private swarm-camera copy.
set -eu
make camera/ztcamera
check_dir=$(mktemp -d /tmp/ztcamera-check-build-XXXXXX)
trap 'rm -rf "$check_dir"' EXIT HUP INT TERM
app_pkgs='xkbcommon wayland-client wayland-egl egl glesv2 cairo pangocairo gdk-pixbuf-2.0'
${CC:-cc} -O1 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -Icore \
    $(pkg-config --cflags $app_pkgs) -o "$check_dir/test-camera" \
    camera/test-camera.c core/zrnmetrocore.c core/zrnmetrogpu.c \
    core/xdg-shell-protocol.c core/viewporter-protocol.c \
    $(pkg-config --libs $app_pkgs) -lm
"$check_dir/test-camera"
# Isolate render-mode camera-roll directories from the real user's captures.
mkdir -p "$check_dir/home"
mkdir -p camera/evidence
HOME="$check_dir/home" camera/ztcamera --render camera/evidence/reference.png
for dial in 0 1 2 3 4; do
    HOME="$check_dir/home" camera/ztcamera --render "camera/evidence/dial-$dial.png" dial "$dial"
done
if HOME="$check_dir/home" camera/ztcamera --render "$check_dir/invalid.png" dial 5; then
    echo 'invalid dial unexpectedly accepted' >&2
    exit 1
fi
printf '%s\n' 'Offline references: camera/evidence/*.png. Live phone screenshots still required.'
