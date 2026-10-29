#!/bin/sh
# Install the media bridge on the phone, as the desktop user (sudo is asked for
# the root steps). Run on the phone, from a directory holding media-bridge.apk,
# media-bridge.token, receiver.py and ztos-waydroid-media.service (what
# build.py and this directory make). Waydroid's session has to be running.
#
#   1. the token, private, where the receiver reads it
#   2. the receiver (a user service, enabled: blocked in accept() between events)
#   3. the app into Android, and its notification-listener grant (Android lets
#      only a listener list other apps' media sessions)
set -eu
here=${ZTOS_MEDIA_DIR:-$(cd "$(dirname "$0")" && pwd)}
apks=$HOME/.local/share/ztos/android/apks
mkdir -p "$HOME/.config/ztos" "$apks" "$HOME/.config/systemd/user"
install -m 600 "$here/media-bridge.token" "$HOME/.config/ztos/media-bridge.token"
sudo install -Dm755 "$here/receiver.py" /usr/local/libexec/ztos-waydroid-media
install -m 644 "$here/ztos-waydroid-media.service" "$HOME/.config/systemd/user/ztos-waydroid-media.service"
systemctl --user daemon-reload
systemctl --user enable --now ztos-waydroid-media.service
install -m 644 "$here/media-bridge.apk" "$apks/media-bridge.apk"
sudo /usr/local/libexec/ztos-waydroid-recover install "$apks/media-bridge.apk"
sudo waydroid shell -- cmd notification allow_listener org.ztos.mediabridge/org.ztos.mediabridge.MediaListener
echo "media bridge installed"
