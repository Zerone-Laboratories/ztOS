#!/bin/sh
# ztMETRO in a window on this machine's desktop (Wayland or X11, any arch), to
# try the shell without a phone. The mouse is a finger (left button), the
# keyboard is the keyboard. The machine's backlight, radios, volume and power
# are left alone (ZTMETRO_NESTED=1, g_nested in ztmetro.c; without it the
# shell will not start in a window).
#
#   scripts/dev/run-nested.sh            build what is missing, then run
#   ZTMETRO_SIZE=720x1480 ZTMETRO_SCALE=1.25 scripts/dev/run-nested.sh
#   scripts/dev/run-nested.sh --fresh    start again from the default tiles and settings
#
# wlroots 0.20 (not packaged on Fedora 44 or most desktops yet) is built once
# into build/wlroots; the shell's files go in build/nested/home, never in ~.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
b=$root/build
wlr=$b/wlroots
nest=$b/nested

pc=$(find "$wlr" -name 'wlroots-0.20.pc' 2>/dev/null | head -n1)
if [ -z "$pc" ]; then
	echo "building wlroots 0.20 into $wlr (once)"
	[ -d "$b/wlroots-src" ] || git clone -q --depth 1 --branch 0.20.0 \
		https://gitlab.freedesktop.org/wlroots/wlroots.git "$b/wlroots-src"
	# from the start each time: a build that stopped half way is not resumed
	rm -rf "$b/wlroots-build" "$wlr"
	meson setup "$b/wlroots-build" "$b/wlroots-src" --prefix="$wlr" \
		-Drenderers=gles2 -Dxwayland=disabled -Dexamples=false
	ninja -C "$b/wlroots-build" install
	pc=$(find "$wlr" -name 'wlroots-0.20.pc' | head -n1)
	if [ -z "$pc" ]; then
		echo "run-nested: wlroots built, but no wlroots-0.20.pc in $wlr" >&2
		exit 1
	fi
fi
lib=$(dirname "$pc")
export PKG_CONFIG_PATH="$lib${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export LD_LIBRARY_PATH="$(dirname "$lib")${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# the shell must build; an app that does not (a library missing) only loses its tile
make -s -C "$root/ztmetro" ztmetro
apps="apps/ztapps settings/ztsettings devtest/zrndevtest contacts/ztcontacts files/ztfiles sound/ztsound"
for a in $apps; do
	make -s -C "$root/ztmetro" "$a" || echo "run-nested: $a did not build, its tile will not start"
done

[ "$1" = --fresh ] && rm -rf "$nest/home"
if [ ! -d "$nest/home" ]; then
	mkdir -p "$nest/home/.local/share/fonts"
	cp -a "$root/os/overlay/etc/skel/." "$nest/home/"
	ln -s "$root/os/overlay/usr/local/share/fonts" "$nest/home/.local/share/fonts/ztos"
fi
mkdir -p "$nest/bin"
for a in $apps; do
	[ -x "$root/ztmetro/$a" ] && ln -sf "$root/ztmetro/$a" "$nest/bin/"
done

export HOME="$nest/home"
export XDG_CONFIG_HOME="$HOME/.config" XDG_DATA_HOME="$HOME/.local/share"
export XDG_STATE_HOME="$HOME/.local/state" XDG_CACHE_HOME="$HOME/.cache"
export PATH="$nest/bin:$PATH"
export ZTMETRO_SCALE="${ZTMETRO_SCALE:-1}"
export ZTMETRO_NESTED=1
exec "$root/ztmetro/ztmetro"
