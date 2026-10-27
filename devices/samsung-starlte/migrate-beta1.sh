#!/bin/sh
# Move a Galaxy S9 running ztOS ALPHA-2 to BETA-1 in place: ztOSIAL, the
# /vendor device layer, the user in /etc/ztos/user.conf, Waydroid's first-boot
# recipe, and the apps and zrnperfd rebuilt against ztOSIAL. Nothing the user
# made is touched (home, Waydroid's Android data, settings); everything replaced
# is kept under /var/backups/ztos-beta1 first.
#
#   sudo sh migrate-beta1.sh <unpacked ztOSMobile tree with ztmetro/ built>
#
# The shell (ztMETRO) is installed but not restarted: it takes over at the
# next reboot, as restarting it stops Waydroid.
set -eu
B=${1:?the unpacked source tree}
BK=/var/backups/ztos-beta1
mkdir -p "$BK"
say() { echo "migrate: $*"; }

say "backing up what changes"
tar cpf "$BK/before.tar" --ignore-failed-read \
	/etc/greetd/config.toml /etc/sudoers.d /etc/polkit-1/rules.d /etc/systemd/system/user@1000.service.d \
	/etc/os-release /usr/local/sbin /usr/local/bin /usr/local/libexec /etc/systemd/system/zrnperfd.service \
	/usr/lib/systemd/system/zrnperfd.service 2>/dev/null || true

say "the user: /etc/ztos/user.conf and its helpers"
install -d /usr/lib/ztos /usr/lib/ztos/python
install -m644 "$B/os/overlay/usr/lib/ztos/user.sh" /usr/lib/ztos/user.sh
install -m644 "$B/os/overlay/usr/lib/ztos/python/ztos_user.py" /usr/lib/ztos/python/ztos_user.py
install -m755 "$B/os/overlay/usr/local/sbin/ztos-user-dropins" /usr/local/sbin/ztos-user-dropins
rm -rf /usr/lib/ztos/user-templates
cp -a "$B/os/overlay/usr/lib/ztos/user-templates" /usr/lib/ztos/
cp -a "$B/rootfs/overlay/usr/lib/ztos/user-templates/." /usr/lib/ztos/user-templates/
if [ ! -e /etc/ztos/user.conf ]; then
	install -d /etc/ztos
	printf 'ZTOS_USER=zerone\nZTOS_UID=1000\nZTOS_GID=1000\nZTOS_HOME=/home/zerone\n' > /etc/ztos/user.conf
fi
cp -a /etc/greetd/config.toml "$BK/greetd.before"
/usr/local/sbin/ztos-user-dropins
if ! cmp -s /etc/greetd/config.toml "$BK/greetd.before"; then
	say "greetd's config came out different: putting the old one back"
	cp -a "$BK/greetd.before" /etc/greetd/config.toml
	exit 1
fi
visudo -c > /dev/null
# the old hand-written drop-in, now ztos-user.conf next to it
if [ -e /etc/systemd/system/user@1000.service.d/ztos-user.conf ]; then
	rm -f /etc/systemd/system/user@1000.service.d/50-audio-realtime.conf
fi
for f in zboot-session zboot-firstboot ztos-fs-recovery-reset; do
	install -m755 "$B/rootfs/overlay/usr/local/sbin/$f" "/usr/local/sbin/$f"
done
install -m755 "$B/devices/samsung-starlte/overlay/usr/local/sbin/zrn-sleepd" /usr/local/sbin/zrn-sleepd

say "Waydroid: scripts and the first-boot recipe"
W=$B/android/waydroid
install -m755 "$W/network.py" /usr/local/libexec/ztos-waydroid-network
install -m755 "$W/recover.sh" /usr/local/libexec/ztos-waydroid-recover
install -m755 "$W/audio-recover.py" /usr/local/libexec/ztos-waydroid-audio
install -m755 "$W/launch.sh" /usr/local/bin/ztos-waydroid-launch
install -m755 "$W/power.sh" /usr/local/bin/ztos-waydroid-power
install -m755 "$W/ztos-waydroid-boot" /usr/local/libexec/ztos-waydroid-boot
install -d /usr/share/ztos-waydroid
for f in install.sh strip-image.sh patch-vendor.sh patch-composer.py waydroid.cfg network.py recover.sh \
		audio-recover.py launch.sh power.sh ztos-waydroid-boot README.md ztos-waydroid-network.service \
		ztos-waydroid-session.service ztos-waydroid-boot.service ztos-waydroid-audio-reconnect.service; do
	[ -e "$W/$f" ] && cp -a "$W/$f" /usr/share/ztos-waydroid/
done
install -m755 "$W/ztos-waydroid-images" /usr/lib/ztos/ztos-waydroid-images
install -m644 "$W/ztos-waydroid-images.service" /usr/lib/systemd/system/ztos-waydroid-images.service
/usr/lib/ztos/ztos-waydroid-images		# images are here already: only marks Android ready
systemctl enable ztos-waydroid-images.service > /dev/null 2>&1

say "ztOSIAL and the /vendor device layer"
( cd "$B/ial" && make > /dev/null && make install PREFIX=/usr > /dev/null )
install -d /vendor/ztos
cp -a "$B/devices/samsung-starlte/vendor/ztos/." /vendor/ztos/
chown -R 0:0 /vendor
systemctl enable ztos-iald > /dev/null 2>&1
ztos-ial check

say "apps and zrnperfd, rebuilt against ztOSIAL"
cd "$B/ztmetro"
grep -E '^\s*install -Dm755 [a-z]+/[a-z-]+ /usr/local/s?bin/' Makefile | awk '{print $3, $4}' | while read -r src dst; do
	[ -f "$src" ] && install -m755 "$src" "$dst.new" && mv "$dst.new" "$dst"
done
install -m755 ztmetro /usr/local/bin/ztmetro.new && mv /usr/local/bin/ztmetro.new /usr/local/bin/ztmetro
install -m644 perfd/zrnperfd.service /etc/systemd/system/zrnperfd.service

say "version"
install -m644 "$B/os/overlay/etc/os-release" /etc/os-release

say "restarting the services that can be (not the shell)"
systemctl daemon-reload
systemctl restart ztos-iald
sleep 2
systemctl restart zrnperfd zrn-sleepd ztos-waydroid-network
sleep 3
for u in ztos-iald zrnperfd zrn-sleepd ztos-waydroid-network greetd; do
	printf '%s %s\n' "$u" "$(systemctl is-active "$u")"
done
say "done; ztMETRO takes the new build at the next reboot"
