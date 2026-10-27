# Sensor hub: start reliably after any reboot (no TWRP pass)

Phone access ONLY via `python /tmp/ztos-phone-control.py run|sudo|put|get` (never plain ssh;
two failed logins = stop). Read sensors/README.md ("Open: the hub does not start from a cold
boot"), sensors/driver/ (zrn-bbd), ztmetro/csf/ztoscsf.c, sensors/hub-snapshot.sh. Vendor
reference: third_party/vendor-kernel (Samsung exynos9810 kernel: ssp/bbd/sensorhub drivers,
DT for gpg0/gph0/gpa pins, regulators). The phone is often unattended: no reboot loops.

## Evidence (2026-10-24, failing boot vs working-after-TWRP, hub-snapshot.sh)
sensors/stock-state/arch-hub-failing-20261024.txt vs arch-hub-working-after-twrp.txt:
- gpg0 DAT 0x10430084: working 0x37, failing 0x23 -> gpg0-2 (GPS_PWR / chip enable) LOW and
  gpg0-4 low when failing.
- gph0 DAT 0x13a80004: working 0x0b, failing 0x1b -> gph0-4 high when failing.
- 0x140500c4 (gpa block, see script labels): working 1, failing 0.
- SPI 0x13ac0014 status: working 0x03000000, failing 0 (no traffic).
- LDO37 same. Kernel: mcu_ipc mbox dumps; sensord ExecStartPre waits 60 s for
  /sys/class/sec/sensorhub/ready then fails; ztoscsf shows "Sensor hub failed to initialize".
- ztoscsf then reboots to recovery itself (`reboot_recovery`), which needs a manual
  `adb reboot` from TWRP -> the phone sits in TWRP / appears to reboot by itself. Fix that too.

## FIRST, before anything else (user-visible breakage)
ztoscsf (ztmetro/csf/ztoscsf.c) answers a hub failure with `ctl command: reboot_recovery` ->
`systemctl reboot --reboot-argument=recovery`, so after a hot reboot the phone "reboots by
itself" into TWRP and stays there (needs adb reboot). Remove that automatic path now (keep a
manual Settings/ctl action if one exists), deploy it, then continue below. The phone was just
sent from TWRP back to ztOS (TWRP pass = hub should be up now).

## Deliver
1. Find what Samsung's kernel does at boot that ours does not (pin config/data of gpg0-2,
   gpg0-4, gph0-4, gpa pins, power sequence timing, reset pulse, regulator order) and make
   zrn-bbd (or the board init) do it on every probe, so the hub comes up after warm reboot,
   forced reboot, and TWRP pass alike. Prefer module-only changes (no boot image flash).
2. Make ztoscsf recover without TWRP: retry the power sequence / module reload with the
   corrected sequence; never reboot into recovery automatically (an unattended phone stuck in
   TWRP is worse than missing sensors). Keep the dialog only if retries fail.
3. Verify: 3x `systemctl reboot`, 1x forced-style reboot if safely possible (e.g. sysrq b /
   watchdog), each time `cat /sys/class/sec/sensorhub/ready` = 1 and sensord active within
   30 s; take hub-snapshot.sh each time. Update sensors/README.md (close or narrow the open
   item) with evidence.
