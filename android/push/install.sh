#!/usr/bin/env bash
# Install zrn-pushd on the phone as root, from this directory (it builds here).
# Also gives the phone's user the two root actions it needs (recover.sh's
# push and microg) and turns it on for the user's session.
set -euo pipefail
[[ $EUID == 0 ]] || { echo 'Usage: sudo ./install.sh' >&2; exit 2; }
. /usr/lib/ztos/user.sh
here=$(cd -- "$(dirname -- "$0")" && pwd)
make -C "$here"
make -C "$here" install
install -Dm644 "$here/README.md" /usr/local/share/doc/zrn-pushd/README.md
# the same rule the OS gets from its user template (ztos-user-dropins)
template=/usr/lib/ztos/user-templates/etc/sudoers.d/zrn-pushd
install -Dm644 "$here/../../rootfs/overlay$template" "$template"
sudoers=$(mktemp)
trap 'rm -f "$sudoers"' EXIT
sed "s|@USER@|$ZTOS_USER|g" "$template" > "$sudoers"
visudo -cf "$sudoers"
install -m440 "$sudoers" /etc/sudoers.d/zrn-pushd
systemctl --global enable zrn-pushd.service
if [[ -S $ZTOS_RUNTIME_DIR/bus ]]; then
    ztos_as_user ztos-android sync >/dev/null || true	# D-Bus names for ATL apps
    ztos_as_user systemctl --user daemon-reload
    ztos_as_user systemctl --user restart zrn-pushd.service
fi
echo 'Installed: zrn-pushd runs in the session (systemctl --user status zrn-pushd).'
