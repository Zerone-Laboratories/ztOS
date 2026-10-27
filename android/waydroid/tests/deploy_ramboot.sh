#!/usr/bin/env bash
# Run as root through the approved phone controller after the native build.
set -euo pipefail
. /usr/lib/ztos/user.sh
work=$ZTOS_HOME/ztmetro-phase2
binary=/usr/local/bin/ztmetro
backup=/usr/local/bin/ztmetro.ram-boot-backup
[[ -x $work/ztmetro && -x $binary ]]
[[ -e "$ZTOS_RUNTIME_DIR/ztos-demoloop.active" ]] && {
	 echo 'Stop DemoLoop before restarting ztMETRO.' >&2
	 exit 1
}
[[ -e $backup ]] || cp -a "$binary" "$backup"
install -m755 "$work/ztmetro" "$binary.ram-boot-new"
mv -T "$binary.ram-boot-new" "$binary"
pkill -x ztmetro || true
sleep 8
if ! pgrep -x ztmetro >/dev/null ||
	! grep -q 'ztMETRO on WAYLAND_DISPLAY' "$ZTOS_HOME/.local/state/ztmetro.log"; then
	install -m755 "$backup" "$binary.ram-boot-rollback"
	mv -T "$binary.ram-boot-rollback" "$binary"
	pkill -x ztmetro || true
	echo 'Shell verification failed; restored the ram-boot backup.' >&2
	exit 1
fi
cmp "$work/ztmetro" "$binary"
sha256sum "$binary" "$backup"
