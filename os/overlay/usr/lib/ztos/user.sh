# ztOS: the phone's user, for scripts that run as root and need that user's
# home, runtime directory or session bus. Source it (sh or bash):
#
#   . /usr/lib/ztos/user.sh
#
# It exports ZTOS_USER, ZTOS_UID, ZTOS_GID, ZTOS_HOME and ZTOS_RUNTIME_DIR
# (/run/user/$ZTOS_UID), and defines ztos_as_user, which runs a command as
# the user with their XDG_RUNTIME_DIR and session bus.
#
# /etc/ztos/user.conf names the user (written at install and first boot).
# Without it, the passwd entry with UID 1000, so a phone installed before the
# file existed keeps working. Safe under set -eu.

ZTOS_USER= ZTOS_UID= ZTOS_GID= ZTOS_HOME=
if [ -r "${ZTOS_USER_CONF:-/etc/ztos/user.conf}" ]; then
	. "${ZTOS_USER_CONF:-/etc/ztos/user.conf}"
fi
# fill what the file left out from passwd: by name, else UID 1000
ztos_pw=$(getent passwd "${ZTOS_USER:-${ZTOS_UID:-1000}}" 2> /dev/null || true)
if [ -n "$ztos_pw" ]; then
	ztos_rest=${ztos_pw#*:*:}
	[ -n "$ZTOS_USER" ] || ZTOS_USER=${ztos_pw%%:*}
	[ -n "$ZTOS_UID" ] || ZTOS_UID=${ztos_rest%%:*}
	ztos_rest=${ztos_rest#*:}
	[ -n "$ZTOS_GID" ] || ZTOS_GID=${ztos_rest%%:*}
	ztos_rest=${ztos_rest#*:*:}
	[ -n "$ZTOS_HOME" ] || ZTOS_HOME=${ztos_rest%%:*}
fi
unset ztos_pw ztos_rest
ZTOS_UID=${ZTOS_UID:-1000}
ZTOS_GID=${ZTOS_GID:-$ZTOS_UID}
ZTOS_HOME=${ZTOS_HOME:-/home/$ZTOS_USER}
ZTOS_RUNTIME_DIR=/run/user/$ZTOS_UID
export ZTOS_USER ZTOS_UID ZTOS_GID ZTOS_HOME ZTOS_RUNTIME_DIR

ztos_as_user() {
	runuser -u "$ZTOS_USER" -- env XDG_RUNTIME_DIR="$ZTOS_RUNTIME_DIR" \
		DBUS_SESSION_BUS_ADDRESS="unix:path=$ZTOS_RUNTIME_DIR/bus" "$@"
}
