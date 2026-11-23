#!/bin/sh
# Run on the host with the S9 in TWRP over USB. Loads zrn_twrp_regdump.ko
# (read-only; it refuses to stay loaded), saves its dump plus TWRP's MIF
# frequency and flexpmu state, then reboots back to ztOS.
#   capture-twrp.sh zrn_twrp_regdump.ko OUTDIR [--stay]
set -u
KO=$1 OUT=$2 STAY=${3:-}
mkdir -p "$OUT"
echo "waiting for TWRP on adb..."
i=0
until [ "$(adb -d shell getprop ro.twrp.version 2>/dev/null | tr -d '\r')" != "" ]; do
	i=$((i + 1)); [ $i -gt 150 ] && { echo "no TWRP on adb after 5 min"; exit 2; }
	sleep 2
done
device=$(adb -d shell getprop ro.product.device | tr -d '\r')
case "$device" in starlte*) ;; *) echo "unexpected device $device; leaving it alone"; exit 2 ;; esac
adb -d shell uname -r | tr -d '\r' > "$OUT/uname.txt"
adb -d push "$KO" /tmp/zrn_twrp_regdump.ko >/dev/null
adb -d shell 'dmesg -c >/dev/null 2>&1; insmod /tmp/zrn_twrp_regdump.ko; echo insmod_rc=$?' | tr -d '\r' > "$OUT/insmod.txt"
adb -d shell dmesg | tr -d '\r' > "$OUT/dmesg.txt"
grep 'zrn-regdump: [0-9a-f]\{8\} ' "$OUT/dmesg.txt" | sed 's/.*zrn-regdump: //' > "$OUT/regdump.txt"
adb -d shell 'for f in /sys/class/devfreq/*/cur_freq; do echo "$f $(cat $f)"; done' | tr -d '\r' > "$OUT/devfreq.txt"
adb -d shell 'mount -t debugfs none /sys/kernel/debug 2>/dev/null; for f in /sys/kernel/debug/flexpmu-dbg/*; do echo "== $f"; cat $f; done' | tr -d '\r' > "$OUT/flexpmu.txt" 2>&1
cat "$OUT/insmod.txt"
echo "$(wc -l < "$OUT/regdump.txt") dump lines in $OUT/regdump.txt"
[ "$STAY" = --stay ] || adb -d reboot
