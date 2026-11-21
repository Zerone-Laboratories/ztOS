#!/bin/bash
# Stage 2 of the boot splash for the workstation tree that builds the phone's
# boot image (~/ztos-kernel-waydroid: build.sh, package.sh). Run it in that
# directory with this stage2/ folder copied anywhere beside it:
#
#   bash /path/to/stage2/apply-on-workstation.sh [--theme dark|light] [--no-uniloader]
#   bash /path/to/stage2/apply-on-workstation.sh --revert
#
# What it changes (the originals are kept once, in .zboot-splash/orig/, and
# every run starts again from them, so running it twice is the same as once):
#   linux/out/.config        CONFIG_CMDLINE + "fbcon=map:9 vt.global_cursor_default=0 logo.nologo"
#   kernel.config            the same, if it is there and has CONFIG_CMDLINE
#                            (where build.sh may take the configuration from)
#   initramfs.list           /init from .zboot-splash/init, plus /bin/zboot-fb,
#                            /splash-dark.raw.gz, /splash-light.raw.gz, /splash-theme
#   loader-source.tar.gz     uniLoader draws the logo instead of its console
#                            (not with --no-uniloader)
# It needs, next to this script: zboot-fb.c, init, uniloader-splash.py,
# make-logo-header.py, and splash-dark.raw.gz + splash-light.raw.gz made on
# the phone (docs/bootsplash/STAGE2.md). Nothing else in the kernel's
# configuration changes, so the phone's modules stay compatible: the script
# checks that CONFIG_CMDLINE is the only line of .config that differs.
# Then: bash build.sh && bash package.sh (detached, as heavy-builds says).
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TREE=$PWD
WORK=$TREE/.zboot-splash
ORIG=$WORK/orig
ADD="fbcon=map:9 vt.global_cursor_default=0 logo.nologo"
INIT_MD5=d7f0b343c411859e798b872cde00f0cb	# initramfs/init the stage-2 init was made from
FRAME_BYTES=17049600			# 1440 x 2960 x 4
CC=$TREE/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-gcc

THEME=dark UNILOADER=1 REVERT=
while [ $# -gt 0 ]; do
	case "$1" in
	--theme) THEME=$2; shift ;;
	--no-uniloader) UNILOADER= ;;
	--revert) REVERT=1 ;;
	*) echo "unknown option $1" >&2; exit 2 ;;
	esac
	shift
done
case "$THEME" in dark|light) ;; *) echo "--theme dark|light" >&2; exit 2 ;; esac

for f in linux/out/.config initramfs.list loader-source.tar.gz build.sh package.sh; do
	[ -e "$TREE/$f" ] || { echo "$TREE/$f is missing: run this in ~/ztos-kernel-waydroid" >&2; exit 1; }
done
FILES=(linux/out/.config initramfs.list loader-source.tar.gz)
[ -f "$TREE/kernel.config" ] && FILES+=(kernel.config)

restore() {
	[ -d "$ORIG" ] || return 0
	for f in "${FILES[@]}"; do
		[ -e "$ORIG/$f" ] && cp -a "$ORIG/$f" "$TREE/$f"
	done
	return 0
}

if [ -n "$REVERT" ]; then
	[ -d "$ORIG" ] || { echo "nothing to revert"; exit 0; }
	restore
	rm -rf "$WORK"
	echo "reverted: ${FILES[*]} are the originals again"
	exit 0
fi

# --- the originals, once; every run starts from them -------------------------
if [ ! -d "$ORIG" ]; then
	for f in "${FILES[@]}"; do
		mkdir -p "$ORIG/$(dirname "$f")"
		cp -a "$TREE/$f" "$ORIG/$f"
	done
	echo "originals kept in $ORIG"
else
	restore
fi

# --- inputs -------------------------------------------------------------------
for f in zboot-fb.c init uniloader-splash.py make-logo-header.py splash-dark.raw.gz splash-light.raw.gz; do
	[ -f "$HERE/$f" ] || { echo "$HERE/$f is missing (the frames are made on the phone)" >&2; exit 1; }
done
for t in dark light; do
	n=$(gzip -dc "$HERE/splash-$t.raw.gz" | wc -c)
	[ "$n" = "$FRAME_BYTES" ] || { echo "splash-$t.raw.gz: $n bytes unpacked, not $FRAME_BYTES" >&2; exit 1; }
done
cur_init=$(awk '$1 == "file" && $2 == "/init" { print $3 }' "$TREE/initramfs.list")
[ -f "$cur_init" ] || { echo "initramfs.list's /init ($cur_init) is not there" >&2; exit 1; }
if [ "$(md5sum < "$cur_init" | cut -d' ' -f1)" != "$INIT_MD5" ]; then
	echo "$cur_init is not the init the stage-2 init was made from ($INIT_MD5):" >&2
	echo "make the stage-2 init again from it (docs/bootsplash/stage2/init.patch)" >&2
	exit 1
fi

mkdir -p "$WORK"
install -m755 "$HERE/init" "$WORK/init"
cp "$HERE/splash-dark.raw.gz" "$HERE/splash-light.raw.gz" "$WORK/"
echo "$THEME" > "$WORK/splash-theme"

# --- the helper ----------------------------------------------------------------
"$CC" -std=c11 -static -Os -s -Wall -o "$WORK/zboot-fb" "$HERE/zboot-fb.c"
if command -v file >/dev/null; then
	file "$WORK/zboot-fb" | grep -q 'statically linked' || { echo "zboot-fb is not static" >&2; exit 1; }
fi

# --- the command line ----------------------------------------------------------
add_cmdline() {	# file
	grep -q '^CONFIG_CMDLINE="' "$1" || { echo "$1: no CONFIG_CMDLINE" >&2; exit 1; }
	grep -q '^CONFIG_CMDLINE=".*fbcon=map:9' "$1" ||
		sed -i "s|^CONFIG_CMDLINE=\"\(.*\)\"$|CONFIG_CMDLINE=\"\1 $ADD\"|" "$1"
}
add_cmdline "$TREE/linux/out/.config"
if [ -f "$TREE/kernel.config" ] && grep -q '^CONFIG_CMDLINE="' "$TREE/kernel.config"; then
	add_cmdline "$TREE/kernel.config"
fi
for f in linux/out/.config kernel.config; do
	[ -f "$TREE/$f" ] || continue
	changed=$(diff "$ORIG/$f" "$TREE/$f" | grep '^[<>]' | grep -cv '^[<>] CONFIG_CMDLINE=' || true)
	[ "$changed" = 0 ] || { echo "$f: more than CONFIG_CMDLINE changed" >&2; restore; exit 1; }
done
grep '^CONFIG_CMDLINE=' "$TREE/linux/out/.config"

# --- the initramfs -------------------------------------------------------------
sed -i "s|^file /init .*|file /init $WORK/init 0755 0 0|" "$TREE/initramfs.list"
cat >> "$TREE/initramfs.list" <<EOF
file /bin/zboot-fb $WORK/zboot-fb 0755 0 0
file /splash-dark.raw.gz $WORK/splash-dark.raw.gz 0644 0 0
file /splash-light.raw.gz $WORK/splash-light.raw.gz 0644 0 0
file /splash-theme $WORK/splash-theme 0644 0 0
EOF

# --- uniLoader -------------------------------------------------------------------
if [ -n "$UNILOADER" ]; then
	python3 -I "$HERE/make-logo-header.py" "$WORK/splash-$THEME.raw.gz" "$WORK/zboot_logo.h"
	tmp=$(mktemp -d)
	trap 'rm -rf "$tmp"' EXIT
	tar -xzf "$ORIG/loader-source.tar.gz" -C "$tmp"
	board=$(find "$tmp" -path '*board/samsung/board-starlte.c' | head -n 1)
	[ -n "$board" ] || { echo "no board/samsung/board-starlte.c in loader-source.tar.gz" >&2; restore; exit 1; }
	grep -q 'zboot failsafe' "$board" ||
		echo "note: this uniLoader has no zboot failsafe block (kernel/uniloader-failsafe.py)"
	python3 -I "$HERE/uniloader-splash.py" "$board" "$WORK/zboot_logo.h"
	(cd "$tmp" && tar -czf "$TREE/loader-source.tar.gz" $(ls -A))
fi

echo
echo "stage 2 applied (theme $THEME${UNILOADER:+, uniLoader draws the logo})."
echo "now: bash build.sh && bash package.sh; to undo: $0 --revert"
