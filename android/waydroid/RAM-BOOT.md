# Waydroid RAM and boot readiness

Date: 2026-10-24. Device: Galaxy S9, 3.5 GiB RAM, zram already configured.

## Changes prepared

- Kept Launcher3 because it is the only verified HOME provider. Kept SystemUI
  because hiding its bars in multi-window mode does not prove its services safe
  to disable. `strip-image.sh` records this decision; no Android package or user
  data was removed.
- Added 150 ms partial and 500 ms complete PSI stall thresholds, heaviest-task
  LMKD selection, and a 1 s kill cooldown. Existing low-RAM mode, PSI mode and
  8/128/256 MiB ART heap limits remain.
- Added a 16-process Android cached-app limit. Instagram's background AppOps are
  `allow` and its standby bucket is `active` because WhatsApp starts its
  `FoaBackupTokenProvider` while linking. Other installed Meta push packages
  receive background AppOps `ignore`, restricted standby and a boot force-stop.
  Packages in `background-apps` or `keep_awake` are exempt.
- Added a global user boot unit. It waits for the Wayland socket, starts the
  Waydroid session, waits at most 45 seconds for Android readiness, then applies
  idle freeze. Failures are logged; the existing lazy launch path remains.
- Changed ztMETRO to hold its splash for Android readiness, continue when ready,
  and continue after a 45 second timeout. A sensor-hub warning no longer blocks
  unattended startup indefinitely.

## Deployment state

Built `ztmetro` on the phone in `/home/zerone/ztmetro-phase2`. Installed it by
atomic rename to `/usr/local/bin/ztmetro`; the previous binary is backed up at
`/usr/local/bin/ztmetro.ram-boot-backup`. The built and installed SHA256 is
`45025513b6c2d295cb16160bc3fedcd34c7b99b21de54db944b2c8204cc57a5d`.

Installed and globally enabled `ztos-waydroid-boot.service` and its helper.
Installed the ztMETRO binary atomically; the active process still runs the prior
image until the next safe restart. Merged the config into
`/var/lib/waydroid/waydroid.cfg` and backed up the original at
`/var/lib/waydroid/ztos-backup/ram-boot/waydroid.cfg`. LMKD and heap properties
take effect after Android restarts. Replaced the network policy with a backup at
`/var/lib/waydroid/ztos-backup/ram-boot/ztos-waydroid-network`, then restarted
only its service. Android accepted cached-process limit 16; Instagram reported
both background AppOps as `ignore` and standby bucket `45` (restricted).

At 10:21 UTC, Waydroid and its session were running; ztMETRO PID 13962 and
Superscan PID 46854 remained alive. Container was not frozen. No reboot or
ztMETRO restart was done because the Superscan scroll task was active. The boot
unit has not run in this session, so boot readiness and new splash behavior
remain unverified. Complete those checks after the scroll task ends.

At 11:20 UTC, Instagram's `RUN_IN_BACKGROUND` and `RUN_ANY_IN_BACKGROUND` AppOps
were changed to `allow`, and its standby bucket was set to `active`, for the
WhatsApp companion-link retry. The persistent network policy now keeps those
settings. Other Meta background restrictions remain unchanged.

## Memory measurements

| Snapshot | Waydroid payload memory | Host free | Host available | State |
|---|---:|---:|---:|---|
| Task baseline, 09:10 UTC | 1.89 GB | 114 MiB | 816 MiB | Superscan open |
| Phone read, 09:39 UTC | 1,186,095,104 bytes | 228 MiB | 1,045 MiB | payload frozen |
| 10:20:50 UTC | 1,520,345,088 bytes | 460.3 MiB | 1,152.7 MiB | Container running; WhatsApp visible in ztMETRO log |
| 10:22 UTC | 1,552,617,472 bytes | 309.8 MiB | 1,131.5 MiB | Container running; WhatsApp open; policy active |

Snapshots have different workloads and cache state; they do not show savings.
The WhatsApp-open snapshot exceeds the `<1 GB` target. No idle measurement was
made, so the `<700 MiB` target remains unverified.

## Boot and launch timings

No before/after boot-to-lock or first WhatsApp-after-boot measurement was made.
Existing records show 24.209 s from a stopped session to the Instagram window,
and one 35 s cold Waydroid launch to `sys.boot_completed`. They are different
measurements and are not a boot-to-lock or WhatsApp baseline. Reboot testing
waits for the active Superscan scroll task to finish.

## Validation and rollback

The final ztMETRO source compiled successfully on the phone. Bash syntax, Python
AST parsing and `git diff --check` passed. Live cache and Instagram policies
were verified. Startup timing, reboot-bound LMKD activation, HOME resolution
after changes, and WhatsApp launch after boot remain unverified.

To roll back the shell, atomically restore
`/usr/local/bin/ztmetro.ram-boot-backup` to `/usr/local/bin/ztmetro`. Restore
`/var/lib/waydroid/ztos-backup/ram-boot/waydroid.cfg` and
`/var/lib/waydroid/ztos-backup/ram-boot/ztos-waydroid-network` to their original
paths. Revert the global boot unit and helper as described in
[README.md](README.md#rollback). Android `device_config` and AppOps rollback
commands are there too. No Android user data was deleted.
