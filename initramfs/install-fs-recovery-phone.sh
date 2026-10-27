#!/bin/bash
set -euo pipefail

BACKUP=/var/backups/fs-recovery
STAGE=/tmp/fs-recovery-stage
MANIFEST=$BACKUP/manifest
if [ -e "$MANIFEST" ]; then
	echo "refusing to replace existing filesystem recovery backups" >&2
	exit 1
fi
mkdir -p "$BACKUP/files" "$BACKUP/absent"

cat > "$MANIFEST" <<'EOF'
etc/fstab
etc/systemd/system/emergency.service.d/ztos.conf
etc/systemd/system/rescue.service.d/ztos.conf
etc/systemd/system/systemd-fsck-root.service.d/ztos.conf
etc/systemd/system/systemd-fsck@.service.d/ztos.conf
etc/systemd/system/ztos-fs-recovery-reset.service
etc/systemd/system/ztos-fs-recovery-rollback.service
etc/systemd/system/graphical.target.wants/ztos-fs-recovery-reset.service
etc/systemd/system/sysinit.target.wants/ztos-fs-recovery-rollback.service
usr/local/sbin/ztos-fsck
usr/local/sbin/ztos-auto-recover
usr/local/sbin/ztos-fs-recovery-reset
usr/local/sbin/ztos-fs-recovery-rollback
usr/lib/systemd/system-shutdown/ztos-root-ro
EOF

while IFS= read -r path; do
	[ -n "$path" ] || continue
	if [ -e "/$path" ] || [ -L "/$path" ]; then
		mkdir -p "$BACKUP/files/$(dirname "$path")"
		cp -a "/$path" "$BACKUP/files/$path"
	else
		mkdir -p "$BACKUP/absent/$(dirname "$path")"
		: > "$BACKUP/absent/$path"
	fi
done < "$MANIFEST"
printf 'install:%s\n' "$(cat /proc/sys/kernel/random/boot_id)" > "$BACKUP/rollback.pending"
chmod 0600 "$BACKUP/rollback.pending"

cp -a "$STAGE/." /
sed -i 's/ binder default,/ binder defaults,/' /etc/fstab
systemctl daemon-reload
systemctl enable ztos-fs-recovery-reset.service ztos-fs-recovery-rollback.service
chmod 0755 /usr/local/sbin/ztos-fsck /usr/local/sbin/ztos-auto-recover \
	/usr/local/sbin/ztos-fs-recovery-reset /usr/local/sbin/ztos-fs-recovery-rollback \
	/usr/lib/systemd/system-shutdown/ztos-root-ro
chmod 0644 /etc/systemd/system/emergency.service.d/ztos.conf \
	/etc/systemd/system/rescue.service.d/ztos.conf \
	/etc/systemd/system/systemd-fsck-root.service.d/ztos.conf \
	/etc/systemd/system/systemd-fsck@.service.d/ztos.conf \
	/etc/systemd/system/ztos-fs-recovery-reset.service \
	/etc/systemd/system/ztos-fs-recovery-rollback.service
