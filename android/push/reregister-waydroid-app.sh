#!/usr/bin/env bash
# Make a Waydroid app register for push again, through microG, under the
# identity zrn-pushd logs in with. Run as root:
#
#   sudo ./reregister-waydroid-app.sh PACKAGE [ACTIVITY]
#
# Firebase keeps an app's push token in the app's own data
# (shared_prefs/com.google.android.gms.appid.xml, "|T|<sender>|*") and hands
# it out again without asking microG for 7 days, or until the app's version
# changes. A token from before microG's current check-in, or from another
# identity, never reaches zrn-pushd. This stops the app, backs that file up to
# /var/backups/waydroid-push/, takes the cached tokens out of it, and starts
# the app, which then registers anew (microG's gcmstatus lists it after).
#
# It changes the app's data: only with the user's consent for that app.
# Undo: stop the app, copy the backup back (keep owner and mode), start it.
set -euo pipefail
[[ $EUID == 0 && $# -ge 1 && $1 =~ ^[a-zA-Z][a-zA-Z0-9_]*(\.[a-zA-Z0-9_]+)+$ ]] || {
	echo 'Usage: sudo ./reregister-waydroid-app.sh PACKAGE [ACTIVITY]' >&2; exit 2; }
package=$1
activity=${2:-}
. /usr/lib/ztos/user.sh
data=$ZTOS_HOME/.local/share/waydroid/data/data/$package
prefs=$data/shared_prefs/com.google.android.gms.appid.xml
[[ -d $data ]] || { echo "$package has no data in Waydroid." >&2; exit 1; }
waydroid shell -- am force-stop "$package"
if [[ -f $prefs ]]; then
	backup=/var/backups/waydroid-push
	install -d -m700 "$backup"
	cp -a "$prefs" "$backup/$package.appid.xml.$(date +%s)"
	# in place: the file keeps its inode, owner, mode and SELinux label
	tmp=$(mktemp)
	grep -v '<string name="|T|' "$prefs" > "$tmp" || true
	cat "$tmp" > "$prefs"
	rm -f "$tmp"
	echo "cached push tokens taken out of $prefs (backup in $backup)"
fi
if [[ -n $activity ]]; then
	waydroid shell -- am start -n "$package/$activity"
else
	waydroid shell -- monkey -p "$package" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1 ||
		echo "start $package by hand (or give its ACTIVITY, e.g. .Main for com.whatsapp)" >&2
fi
echo "Then: sqlite3 $ZTOS_HOME/.local/share/waydroid/data/data/com.google.android.gms/databases/gcmstatus 'select package_name from registrations'"
