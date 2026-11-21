#!/bin/sh
# migrate-coru.sh: move the phone's running system from the watchdog's old
# name to its new one (run as root on the phone, after the new files are
# installed: ztmetro's coru/ztoscoru, coru/ztoscoru.service, coru/components,
# and the device overlay's units that name it, such as ztos-gnss.service).
#
#   ztoscsf (ztoscoresensorframework.service)  -> ztoscoru (ztoscoru.service)
#   /etc/ztoscoresensorframework/components    -> /etc/ztoscoru/components
#   /var/lib/ztoscsf (the modules' marks)      -> /var/lib/ztoscoru
#
# ztOSCOR-U, the ztOS Central Operations Register Unit, is the watchdog; the
# name ztOSCoreSensorFramework stays with the sensor hub and the sensors
# (ztoscsf-sensorhub, ztoscsf-sensord), which this script never touches.
#
# Nothing but the watchdog is stopped or restarted. Stopping it ends its own
# process (and a modprobe or systemctl it may be running at that instant,
# which is why it waits for the old one to be idle); the units it started run
# in their own cgroups and keep running. It waits for the sensor hub to be
# ready first: a new watchdog meeting a hub that is not ready runs the hub's
# power recovery, which restarts the hub.
#
# Safe to run again. Refuses, leaving the old watchdog running, when the new
# binary or unit is missing. If the new one does not come up, the old one is
# started again. The old files are kept in /var/backups/ztos-pre-coru.
#
#   migrate-coru.sh             migrate
#   migrate-coru.sh --rollback  back to ztoscsf (stops ztoscoru)
set -eu
[ "$(id -u)" = 0 ] || { echo "as root" >&2; exit 1; }

OLD=ztoscoresensorframework.service NEW=ztoscoru.service
OLD_BIN=/usr/local/sbin/ztoscsf NEW_BIN=/usr/local/sbin/ztoscoru
OLD_UNIT=/etc/systemd/system/$OLD NEW_UNIT=/etc/systemd/system/$NEW
OLD_LIST=/etc/ztoscoresensorframework/components NEW_LIST=/etc/ztoscoru/components
OLD_MARKS=/var/lib/ztoscsf NEW_MARKS=/var/lib/ztoscoru
BACKUP=/var/backups/ztos-pre-coru
HUB=/sys/class/sec/sensorhub/ready

did() { echo "  $*"; }

# The sensor hub is ready, stopped for this boot, or not on this phone.
# Waits up to 90 s (it may be in the middle of a recovery).
hub_settled() {
	[ -e "$HUB" ] || return 0
	i=0
	while [ $i -lt 90 ]; do
		[ "$(cat "$HUB" 2>/dev/null)" = 1 ] && return 0
		[ -e /run/ztoscsf-sensorhub.stopped ] || [ -e /run/sensorhub.stopped ] && return 0
		[ $i = 0 ] && echo "waiting for the sensor hub to be ready (up to 90 s)"
		sleep 1; i=$((i + 1))
	done
	echo "the sensor hub is not ready: a new watchdog would run its recovery and restart it." >&2
	echo "Nothing was changed. Reboot first, or stop the hub for this boot" >&2
	echo "(touch /run/ztoscsf-sensorhub.stopped) and run this again." >&2
	return 1
}

# A watchdog unit's process has run 30 s (its modules are loaded) and runs
# nothing but itself (no modprobe, systemctl or recovery step in flight).
# Waits up to 60 s.
idle() {
	systemctl is-active -q "$1" || return 0
	i=0
	while [ $i -lt 60 ]; do
		pid=$(systemctl show -p MainPID --value "$1")
		age=$(ps -o etimes= -p "${pid:-0}" 2>/dev/null | tr -d ' ')
		cg=/sys/fs/cgroup$(systemctl show -p ControlGroup --value "$1")/cgroup.procs
		procs=$(cat "$cg" 2>/dev/null | wc -l)
		[ "${age:-0}" -ge 30 ] && [ "$procs" -le 1 ] && return 0
		sleep 1; i=$((i + 1))
	done
	echo "$1 stayed busy for 60 s; nothing was changed" >&2
	return 1
}

if [ "${1:-}" = --rollback ]; then
	echo "rollback to ztoscsf:"
	for f in "$OLD_BIN" "$OLD_UNIT"; do
		[ -e "$f" ] || [ ! -e "$BACKUP/$(basename "$f")" ] || { cp -p "$BACKUP/$(basename "$f")" "$f"; did "restored $f"; }
		[ -e "$f" ] || { echo "$f is gone and not in $BACKUP: cannot roll back" >&2; exit 1; }
	done
	if [ ! -e "$OLD_LIST" ] && [ -e "$BACKUP/components" ]; then
		mkdir -p "${OLD_LIST%/*}"; cp -p "$BACKUP/components" "$OLD_LIST"; did "restored $OLD_LIST"
	fi
	if [ ! -e "$OLD_LIST" ] && [ -e "$NEW_LIST" ]; then
		mkdir -p "${OLD_LIST%/*}"; cp -p "$NEW_LIST" "$OLD_LIST"; did "copied $NEW_LIST to $OLD_LIST"
	fi
	hub_settled
	idle "$NEW"
	systemctl daemon-reload
	if systemctl is-enabled -q "$NEW" 2>/dev/null || systemctl is-active -q "$NEW"; then
		systemctl disable --now "$NEW"; did "stopped and disabled $NEW"
	fi
	if [ -d "$NEW_MARKS" ] && [ ! -e "$OLD_MARKS" ]; then
		mv "$NEW_MARKS" "$OLD_MARKS"; sync; did "moved $NEW_MARKS to $OLD_MARKS"
	fi
	# (left out of the way, so a unit that restarts ztoscoru.service cannot
	# start a second watchdog next to the old one)
	if [ -e "$NEW_UNIT" ]; then
		mkdir -p "$BACKUP"; mv "$NEW_UNIT" "$BACKUP/$NEW.rolled-back"; did "moved $NEW_UNIT to $BACKUP"
	fi
	rm -rf "/etc/systemd/system/$NEW.d"
	systemctl daemon-reload
	systemctl enable "$OLD"
	systemctl start "$OLD"; did "enabled and started $OLD"
	[ ! -x /usr/local/sbin/zrn-crash-apply ] || { /usr/local/sbin/zrn-crash-apply > /dev/null && did "ran zrn-crash-apply"; }
	refs=$(grep -ls "$NEW" /etc/systemd/system/*.service 2>/dev/null || true)
	[ -z "$refs" ] || echo "note: these units still name $NEW (put back their old copies): $refs"
	echo "done: $OLD $(systemctl is-active "$OLD")"
	exit 0
fi

echo "migrate to ztoscoru (ztOSCOR-U):"
# ---- refuse before anything changes ----
[ -x "$NEW_BIN" ] || { echo "$NEW_BIN is not installed; nothing was changed" >&2; exit 1; }
[ -f "$NEW_UNIT" ] || { echo "$NEW_UNIT is not installed; nothing was changed" >&2; exit 1; }
grep -q "ExecStart=$NEW_BIN" "$NEW_UNIT" || { echo "$NEW_UNIT does not run $NEW_BIN; nothing was changed" >&2; exit 1; }
grep -q ztOSCOR-U "$NEW_BIN" || { echo "$NEW_BIN is not the ztOSCOR-U build; nothing was changed" >&2; exit 1; }
rc=0; "$NEW_BIN" status > /dev/null 2>&1 || rc=$?
[ $rc -le 1 ] || { echo "$NEW_BIN does not run here (exit $rc: built for another machine?); nothing was changed" >&2; exit 1; }
[ -e "$NEW_LIST" ] || [ -e "$OLD_LIST" ] || { echo "neither $NEW_LIST nor $OLD_LIST is there; nothing was changed" >&2; exit 1; }
refs=$(grep -ls "$OLD" /etc/systemd/system/*.service /etc/systemd/system/*.service.d/*.conf 2>/dev/null |
	grep -v -e "/$OLD\$" -e "/$OLD.d/" || true)
if [ -n "$refs" ]; then
	echo "these units still name $OLD; install their new copies first (nothing was changed):" >&2
	echo "$refs" >&2
	exit 1
fi
hub_settled
idle "$OLD"

# ---- the list ----
if [ ! -e "$NEW_LIST" ]; then
	mkdir -p "${NEW_LIST%/*}"; cp -p "$OLD_LIST" "$NEW_LIST"; did "copied $OLD_LIST to $NEW_LIST"
elif [ -e "$OLD_LIST" ] && ! cmp -s "$OLD_LIST" "$NEW_LIST"; then
	did "kept $NEW_LIST; it differs from the old list (old one kept in $BACKUP):"
	diff "$OLD_LIST" "$NEW_LIST" | sed 's/^/    /' || true
fi

# ---- the new unit on, before the old one goes ----
systemctl daemon-reload
systemctl enable "$NEW" 2> /dev/null || true
systemctl is-enabled -q "$NEW" || { echo "$NEW did not enable; the old watchdog is still running" >&2; exit 1; }
did "enabled $NEW"
if [ -e "$OLD_UNIT" ] && { systemctl is-enabled -q "$OLD" 2>/dev/null || systemctl is-active -q "$OLD"; }; then
	# (never two watchdogs enabled for the next boot)
	if ! systemctl disable --now "$OLD"; then
		systemctl disable "$NEW" 2> /dev/null || true
		systemctl start "$OLD" || true
		echo "$OLD did not stop; $NEW disabled again, the old watchdog kept" >&2
		exit 1
	fi
	did "stopped and disabled $OLD"
fi
rm -rf /run/ztoscsf	# its status, now stale (the new one makes /run/ztoscoru)

# ---- the marks (they keep a module that once froze the phone from loading) ----
if [ -d "$OLD_MARKS" ] && [ ! -e "$NEW_MARKS" ]; then
	mv "$OLD_MARKS" "$NEW_MARKS"; sync; did "moved $OLD_MARKS to $NEW_MARKS"
elif [ -d "$OLD_MARKS" ]; then
	for f in "$OLD_MARKS"/*; do
		[ -e "$f" ] || continue
		[ -e "$NEW_MARKS/${f##*/}" ] || { mv "$f" "$NEW_MARKS/"; did "moved mark ${f##*/} to $NEW_MARKS"; }
	done
	sync
	rm -rf "$OLD_MARKS"; did "removed $OLD_MARKS (merged into $NEW_MARKS)"
fi

# ---- start the new one; the old one comes back if it does not stay up ----
if systemctl is-active -q "$NEW"; then
	did "$NEW was already running"
else
	systemctl start "$NEW" || true
	sleep 8
	if ! systemctl is-active -q "$NEW" || [ "$(systemctl show -p NRestarts --value "$NEW")" != 0 ]; then
		journalctl -b -u "$NEW" -n 20 --no-pager >&2 || true
		systemctl disable "$NEW" 2>/dev/null || true
		systemctl stop "$NEW" 2>/dev/null || true
		if [ ! -e "$OLD_UNIT" ] || [ ! -e "$OLD_BIN" ]; then
			echo "$NEW did not stay up, and the old watchdog is gone: run --rollback" >&2
			exit 1
		fi
		echo "$NEW did not stay up; starting the old watchdog again" >&2
		[ -d "$NEW_MARKS" ] && [ ! -e "$OLD_MARKS" ] && mv "$NEW_MARKS" "$OLD_MARKS" && sync
		systemctl enable "$OLD" || true
		systemctl start "$OLD" || true
		exit 1
	fi
	did "started $NEW"
fi

# ---- the old files, last (kept in $BACKUP for --rollback) ----
mkdir -p "$BACKUP"
for f in "$OLD_UNIT" "$OLD_BIN" "$OLD_LIST"; do
	[ -e "$f" ] || continue
	cp -p "$f" "$BACKUP/" && rm -f "$f" && did "removed $f (kept in $BACKUP)"
done
rmdir "${OLD_LIST%/*}" 2>/dev/null || true
rm -rf "/etc/systemd/system/$OLD.d"

# ---- the crash policy drop-in for the new unit ----
if [ -x /usr/local/sbin/zrn-crash-apply ]; then
	/usr/local/sbin/zrn-crash-apply > /dev/null && did "ran zrn-crash-apply"
fi
systemctl daemon-reload

grep -qs ztoscoresensorframework /etc/polkit-1/rules.d/50-ztos-devices.rules &&
	echo "note: /etc/polkit-1/rules.d/50-ztos-devices.rules still names $OLD: install the new rootfs overlay copy"
echo "done: $NEW $(systemctl is-active "$NEW"), $(systemctl is-enabled "$NEW") at boot;" \
	"$OLD $(systemctl is-active "$OLD" 2>/dev/null || true)"
