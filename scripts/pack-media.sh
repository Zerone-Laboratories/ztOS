#!/bin/sh
# Pack the wallpapers and sounds into build/ztos-media.tar.xz (xz -9), which the
# installer unpacks into the image. Local only: build/ is ignored and these are
# third-party files not ours to redistribute, so the pack never goes in git.
# Needs build/wallpapers (os/wallpapers/make-wallpapers.py) and the local sound
# pack in os/overlay.
set -eu
cd "$(dirname "$0")/.."
W=build/wallpapers/backgrounds/ztos
A=os/overlay/usr/local/share/sounds/ztos
[ -d "$W" ] || { echo "pack-media: no $W" >&2; exit 1; }
[ -d "$A/phone" ] || { echo "pack-media: no $A/phone" >&2; exit 1; }
OUT=build/ztos-media.tar.xz
tar --owner=0 --group=0 --sort=name -cf - \
	--transform 's|^build/wallpapers/backgrounds/ztos|usr/local/share/backgrounds/ztos|' \
	--transform 's|^os/overlay/usr/local/share/sounds/ztos|usr/local/share/sounds/ztos|' \
	"$W" "$A" | xz -9 -T0 > "$OUT.tmp"
xz -t "$OUT.tmp"
mv "$OUT.tmp" "$OUT"
ls -l "$OUT"
