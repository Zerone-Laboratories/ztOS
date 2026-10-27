#!/usr/bin/env bash
# Install on the phone as root. Rebuild system.img from stock unless --config-only is supplied.
set -euo pipefail
[[ $EUID == 0 && $# -le 1 && ( ${1:---strip} == --strip || ${1:-} == --config-only ) ]] || {
    echo 'Usage: sudo ./install.sh [--strip|--config-only]' >&2; exit 2;
}
. /usr/lib/ztos/user.sh
here=$(cd -- "$(dirname -- "$0")" && pwd)
images=/usr/share/waydroid-extra/images
backup=/var/lib/waydroid/ztos-backup
for tool in waydroid nft lxc-info lxc-attach nsenter e2fsck resize2fs debugfs python3 visudo; do
    command -v "$tool" >/dev/null
done
if [[ ${1:-} == --config-only ]] && ! debugfs -R 'stat /system/etc/init/hw/init.zygote64.rc' "$images/system.img" 2>/dev/null | grep -q '^Inode:'; then
    echo '--config-only requires an image already built by strip-image.sh.' >&2
    exit 1
fi
mkdir -p "$backup"
cp -an /var/lib/waydroid/waydroid.cfg "$backup/waydroid.cfg"
# Stop Android, without removing or changing its user data.
if [[ -S $ZTOS_RUNTIME_DIR/bus ]]; then
    ztos_as_user timeout 25 waydroid session stop || true
    ztos_as_user systemctl --user stop ztos-waydroid-session.service || true
fi
systemctl stop waydroid-container.service
cp -an --reflink=auto --sparse=always "$images/system.img" "$images/system.img.stock"
if [[ ${1:---strip} == --strip ]]; then
    output=$images/system.img.lean.$$
    "$here/strip-image.sh" "$images/system.img.stock" "$output" | tee "$backup/strip.log"
    mv -T "$output" "$images/system.img"
fi
python3 - "$here/waydroid.cfg" <<'PY'
import configparser
import os
import sys
path = '/var/lib/waydroid/waydroid.cfg'
config = configparser.ConfigParser()
config.read(path)
new = configparser.ConfigParser()
new.read(sys.argv[1])
for section in new.sections():
    if not config.has_section(section):
        config.add_section(section)
    for key, value in new[section].items():
        config[section][key] = value
with open(path + '.ztos-new', 'w') as output:
    config.write(output)
os.chmod(path + '.ztos-new', 0o644)
os.replace(path + '.ztos-new', path)
PY
"$here/patch-vendor.sh"
install -Dm644 "$here/README.md" /usr/local/share/doc/ztos-waydroid/README.md
install -Dm755 "$here/network.py" /usr/local/libexec/ztos-waydroid-network
install -Dm755 "$here/recover.sh" /usr/local/libexec/ztos-waydroid-recover
install -Dm755 "$here/audio-recover.py" /usr/local/libexec/ztos-waydroid-audio
systemctl disable --now ztos-waydroid-audio.service 2>/dev/null || true
rm -f /etc/systemd/system/ztos-waydroid-audio.service
install -Dm644 "$here/ztos-waydroid-audio-reconnect.service" /etc/systemd/user/ztos-waydroid-audio-reconnect.service
install -Dm755 "$here/launch.sh" /usr/local/bin/ztos-waydroid-launch
install -Dm755 "$here/power.sh" /usr/local/bin/ztos-waydroid-power
install -Dm755 "$here/ztos-waydroid-boot" /usr/local/libexec/ztos-waydroid-boot
install -Dm644 "$here/ztos-waydroid-network.service" /etc/systemd/system/ztos-waydroid-network.service
install -Dm644 "$here/ztos-waydroid-session.service" /etc/systemd/user/ztos-waydroid-session.service
install -Dm644 "$here/ztos-waydroid-boot.service" /etc/systemd/user/ztos-waydroid-boot.service
mkdir -p /etc/systemd/system/waydroid-container.service.d
cat > /etc/systemd/system/waydroid-container.service.d/ztos-recovery.conf <<'UNIT'
[Service]
Restart=on-failure
RestartSec=3
UNIT
sudoers=$(mktemp)
trap 'rm -f "$sudoers"' EXIT
cat > "$sudoers" <<RULES
$ZTOS_USER ALL=(root) NOPASSWD: /usr/local/libexec/ztos-waydroid-recover audio, /usr/local/libexec/ztos-waydroid-recover prepare, /usr/local/libexec/ztos-waydroid-recover ready, /usr/local/libexec/ztos-waydroid-recover recover, /usr/local/libexec/ztos-waydroid-recover freeze, /usr/local/libexec/ztos-waydroid-recover unfreeze, /usr/local/libexec/ztos-waydroid-recover install *, /usr/local/libexec/ztos-waydroid-recover remove *, /usr/local/libexec/ztos-waydroid-recover close *
RULES
visudo -cf "$sudoers"
install -m440 "$sudoers" /etc/sudoers.d/ztos-waydroid
waydroid upgrade -o
systemctl daemon-reload
systemctl enable --now waydroid-container.service ztos-waydroid-network.service
systemctl restart ztos-waydroid-network.service
systemctl --global enable ztos-waydroid-audio-reconnect.service
systemctl --global enable ztos-waydroid-boot.service
if [[ -S $ZTOS_RUNTIME_DIR/bus ]]; then
    ztos_as_user systemctl --user daemon-reload
    ztos_as_user systemctl --user start ztos-waydroid-audio-reconnect.service
fi
echo 'Installed. Waydroid starts at desktop boot; launch apps with ztos-waydroid-launch PACKAGE.'
