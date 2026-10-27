#!/bin/bash
set -euo pipefail

WRAPPER=${1:-/usr/local/sbin/ztos-fsck}
for pair in '0 clean' '1 corrected' '2 reboot' '3 reboot' '4 forced-repair' '8 forced-repair'; do
	read -r rc expected <<<"$pair"
	actual=$(bash -p "$WRAPPER" --classify-rc "$rc")
	[ "$actual" = "$expected" ] || { echo "rc $rc: expected $expected, got $actual" >&2; exit 1; }
done

[ "$EUID" -eq 0 ] || { echo "run as root on the phone" >&2; exit 1; }
for tool in truncate losetup mkfs.ext4 debugfs tune2fs dumpe2fs dd; do
	command -v "$tool" >/dev/null || { echo "missing $tool" >&2; exit 1; }
done
image=$(mktemp /tmp/ztos-fsck-loop.XXXXXX.img)
loop=
cleanup() {
	[ -z "$loop" ] || losetup -d "$loop" 2>/dev/null || true
	rm -f "$image"
}
trap cleanup EXIT
truncate -s 96M "$image"
loop=$(losetup --find --show "$image")
mkfs.ext4 -q -F "$loop"
root_block=$(debugfs -R 'blocks <2>' "$loop" 2>/dev/null | awk '{print $1}')
block_size=$(dumpe2fs -h "$loop" 2>/dev/null | awk -F: '/^Block size:/ {gsub(/ /, "", $2); print $2}')
[ -n "$root_block" ] && [ -n "$block_size" ] || { echo "could not locate test root directory block" >&2; exit 1; }
dd if=/dev/zero of="$loop" bs="$block_size" seek="$root_block" count=1 conv=notrunc status=none
tune2fs -E force_fsck "$loop" >/dev/null 2>&1
bash -p "$WRAPPER" "$loop"
log=$(ls -1t /run/ztos-fsck/*-$(basename "$loop").log | head -1)
grep -Eq 'Pass 1:|UNEXPECTED INCONSISTENCY|running forced repair' "$log"
echo "PASS: rc mapping and corrupted loop ext4 repair; log=$log"
