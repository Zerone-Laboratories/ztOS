# Filesystem recovery design

## Boot fsck

`systemd-fsck-root.service` and `systemd-fsck@.service` use
`/usr/local/sbin/ztos-fsck`. It runs e2fsck preen (`-p`); return codes 4 or
higher trigger forced repair (`-f -y`). Codes 2 and 3 request a reboot.
The wrapper always returns success to systemd, so fsck errors do not activate
`emergency.target`. It streams all e2fsck output to `/dev/kmsg`, keeps a
per-boot copy under `/run/ztos-fsck`, and copies root-device logs to
`/var/log/ztos-fsck` after a successful remount read-write. The unit overrides
are in `rootfs/overlay/etc/systemd/system/`.

## Emergency and rescue

Both shell units start `/usr/local/sbin/ztos-auto-recover`, with no tty input.
It logs failed units and the last 100 journal lines to kmsg, remounts root
read-only, runs `e2fsck -f -y`, saves the output, then reboots. Recovery count
lives in `/var/lib/ztos-fs-recovery/attempts` and is reset after ztMETRO starts.
On the third attempt, it asks systemd to isolate `graphical.target` while
ignoring dependency failures and marks `/run/ztos-fs-recovery/notify` for a
ztMETRO notification. `/run/zboot.hold` remains owned by the existing zboot
USB service; these changes do not remove or rewrite that behavior.

The persistent counter needs root to become writable after fsck. If the root
filesystem cannot be remounted read-write, the script logs that the attempt
count could not persist. This phone has no verified writable EFI variable
store, and pstore is a kernel crash log, not a writable counter. Thus a fully
unmountable root cannot guarantee the three-boot bound with available
persistent storage. The e2fsck operation runs against the root device while
root is mounted read-only; this is the same root-device repair context used by
the boot fsck, but e2fsck can still warn that a mounted filesystem is in use.

## Shutdown and mount policy

`rootfs/overlay/usr/lib/systemd/system-shutdown/ztos-root-ro` syncs and asks
the kernel to remount root read-only immediately before final shutdown. It
logs the result to kmsg. `errors=remount-ro` is explicit in the phone fstab:
new ext4 errors stop further writes while retaining a bootable filesystem for
recovery. systemd-shutdown logs are not available in the current pstore dump,
so this hook is precautionary; it does not prove the earlier shutdown reached
the final unmount. The one current boot's pstore file contains console output,
not a shutdown trace. The journal records the root ext4 error warning after
the TWRP repair. No ext4 superblock first/last error fields were reported by
`dumpe2fs -h`; this does not establish the original corruption cause.

The current systemd watchdog is `S3C2410 Watchdog`, with a 30-second hardware
timeout. Existing zrn modules and shutdown path were not unloaded or changed;
available logs do not identify a module that blocked the previous shutdown.
The reported corrupt directory was the user's WhatsApp `node_modules/lodash`;
no evidence ties it to a specific service or module.

## Validation and limits

`initramfs/test-fs-recovery-loop.sh` checks wrapper return-code classification,
then creates an ext4 image, attaches a loop device, corrupts its root directory
block, marks it for checking, and runs the wrapper against that image. It never
targets the real root. It passed on the phone with e2fsck 1.47.4; the test log
was removed after validation. Run it as root on the phone after installing
the overlay:

```sh
bash initramfs/test-fs-recovery-loop.sh
```

`ztos-auto-recover --dry-run` prints the recovery sequence without side effects.
The emergency path has not been isolated on the live phone because its only
automatic recovery is reboot. The current phone's failed unit is
`dev-binderfs.mount`; its fstab `default` option is rejected by binderfs
(`Unknown parameter 'default'`). This is unrelated to ext4 recovery and is
reported rather than silently masked.

## Recurrence (2026-10-24 06:46 UTC) — open
The same directory, inode 524468 (~/.local/share/ztos/whatsapp/node_modules/lodash), was
corrupted again after the TWRP repair (`block #1, offset 0: directory corrupted`). ztos-fsck
salvaged it and rebooted (log /var/log/ztos-fsck/20261024-064611-sda25.log). A repeat on the
same inode points at whatever writes that directory or at the UFS write path (unclean reset
while it is written), not a random event. To investigate: what process writes
~/.local/share/ztos/whatsapp (node install?), whether the corruption follows a hard reset
(watchdog / forced reboot / ztoscsf reboot_recovery), dumpe2fs error fields, UFS driver logs.
