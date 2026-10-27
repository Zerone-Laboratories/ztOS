# Follow-up: install fs-recovery on the phone (see FS-RECOVERY-TASK.md, FS-RECOVERY.md)
Phone access ONLY via `python /tmp/ztos-phone-control.py` (never plain ssh; two failed logins = stop).
1. Fix the fstab binderfs line (option `default` -> `defaults`) in repo overlay/installer and
   on the phone so the system is not degraded.
2. Install the fs-recovery files on the phone; back up every replaced file under
   /var/backups/fs-recovery/.
3. Run systemd-analyze verify and the dry-run mode on the phone.
4. Before rebooting, add an automatic rollback (e.g. a oneshot that restores the backups if
   the previous boot did not reach the counter reset). Then reboot once normally and verify:
   boots to ztMETRO, `systemctl is-system-running` = running, recovery units ran in normal
   mode with no side effects, counter-reset service ran.
5. Make the attempt counter work while root stays read-only (CACHE partition
   /dev/block/by-name/CACHE, an efivar, or pstore marker — pick the safest) so the
   3-attempt bound always holds.
Update initramfs/FS-RECOVERY.md with install + reboot evidence.
