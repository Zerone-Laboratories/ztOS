# Automatic filesystem recovery at boot (no keyboard)

Phone access ONLY via `python /tmp/ztos-phone-control.py run|sudo|put|get` (never plain ssh;
two failed logins = stop). The phone has no keyboard and is often unattended: it must NEVER
stop at a prompt. Do not flash a boot image unless the systemd-side fix is impossible (say why).
Do not delete user data.

## What happened (2026-10-24, verified from the phone's journal)
- zboot initramfs (initramfs/init) mounts the root `LABEL=ZR_ARCH` (/dev/sda25, USERDATA) ro and
  switch_roots. systemd-fsck-root then ran e2fsck in preen mode and failed:
  `Directory inode 524468, block #1, offset 0: directory corrupted` /
  `UNEXPECTED INCONSISTENCY; RUN fsck MANUALLY` / `fsck failed with exit status 4` ->
  emergency mode -> "Press Enter to continue" (user saw it; it happened before too).
- Repaired from TWRP with the system's own e2fsck 1.47.4 (`-f -y`, rc=1, second pass rc=0).
  TWRP's e2fsck is 1.43.3: too old to trust; never use it.
- The damaged dir was ~/.local/share/ztos/whatsapp/node_modules/lodash (files in lost+found).
- Previous boots logged a normal shutdown, but the journal stops before the final unmount;
  memory notes a stuck module once blocked shutdown. Cause of the corruption is unproven.

## Deliver
1. Root fsck never drops to a prompt: drop-in for systemd-fsck-root.service (and
   systemd-fsck@.service for any other ext4 in /etc/fstab) running a ztOS wrapper
   (/usr/local/sbin/ztos-fsck) that runs preen, and on rc>=4 runs `e2fsck -f -y`; logs the
   full output to the kernel log (/dev/kmsg, so pstore/ramoops keeps it) and to
   /var/log/ztos-fsck/ once root is rw; on rc 2/3 ("reboot required") reboots. Never fails the
   unit so emergency mode is not triggered by fsck.
2. Emergency/rescue mode never waits for a key: drop-ins for emergency.service and
   rescue.service replace sulogin with /usr/local/sbin/ztos-auto-recover: log why (systemctl
   --failed, journal tail to kmsg), remount root ro, `e2fsck -f -y` the root, then reboot.
   Bound it: count attempts somewhere that survives reboot and is writable when root is not
   (e.g. a pstore/efivar/zboot mechanism, or the root fs itself after repair); after 3
   consecutive failed recoveries, boot on anyway with the failing units skipped (or reach
   graphical target) and show a ztMETRO notification, rather than looping forever. Reset the
   counter after a successful boot (when ztMETRO is up).
   Also keep the existing zboot deadman behaviour working (/run/zboot.hold).
3. Shutdown hardening: make sure root is remounted ro / synced before the final reset
   (check systemd-shutdown logs via pstore, the watchdog, zrn modules that block rmmod), and
   that `errors=` mount behaviour is sane (keep `continue` or `remount-ro`, explain).
   Find any evidence for what corrupted the fs (pstore/ramoops /sys/fs/pstore,
   dumpe2fs error fields: first/last error time/function/inode).
4. Test without breaking the phone: unit tests of the wrapper rc mapping with a loop-device
   ext4 image you corrupt on the phone (debugfs/dd on the image, NOT the real root); test the
   emergency path by `systemctl isolate emergency.target`-like simulation only if you have a
   guaranteed recovery (the reboot) — prefer `systemd-analyze verify` + a dry-run mode.
   Put files in the repo under rootfs/overlay/ (and the installer if it builds fstab/units).
Write initramfs/FS-RECOVERY.md with design, test evidence and limits.
