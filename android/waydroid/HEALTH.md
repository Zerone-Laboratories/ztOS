# Waydroid health and battery audit

Date: 2026-10-24. Device: starlte. All device access used the approved
`python /tmp/ztos-phone-control.py run|sudo|put|get` controller.

## Status: fixes deployed; acceptance measurements blocked

The requested uninterrupted 30-minute Android health check and two 20-minute
screen-off battery trials are **not complete**. A clean phone reboot at 13:43
UTC, a Waydroid restart at 13:46:40–13:46:46, and repeated screen-on activity
interrupted collection. These actions were not requested by this audit. The
compositor log shows concurrent app launches and navigation. Both A/B attempts
aborted after one minute when the screen turned on; neither reached the frozen
phase. No battery savings or healthy-for-30-minutes claim is justified.

The phone needs a reserved 40-minute screen-off window for the A/B test, plus
an uninterrupted 30-minute Android use window for health acceptance. WhatsApp
currently blocks registration, so normal messaging and its input test also
need that existing application issue resolved.

## Changes and deployment

- `network.py` applies `settings put global hide_error_dialogs 1` after Android
  boot readiness, once per container boot. `install.sh` already installs this
  helper and enables its service, making the setting reproducible. Readback
  after the phone reboot and the later container restart was `1`.
- The network helper now checks the messaging exemption before querying
  `waydroid.open_windows` and `persist.waydroid.suspend`. With a messaging
  exemption active, this avoids two Android process launches every five
  seconds. Routing repair and the idle freeze path remain available. The
  deployed network service is active. Its original helper is backed up at
  `/var/tmp/ztos-health/network.before` on the phone and in
  `results/health-network-before.py` in this repository.
- Added `rootfs/overlay/etc/systemd/system/zrn-cpuidle.service.d/power.conf`
  and installed the same drop-in on the phone. It selects CPU mask `0x1f`
  with SICD and RELAX disabled. The loader checks for the kernel's
  `exynos9810_suspend_mpidr_normalize` fix before accepting a wider mask;
  that symbol exists on this phone.

Cpuidle was initially loaded for CPUs 2–3 (`0x0c`), while the screen-off policy
left CPUs 0–1 and 4 online. Thus the loaded driver did not cover those idle
CPUs. A normal `rmmod` refused because the module was in use; no forced unload
was attempted. The subsequent independently initiated reboot loaded `0x1f`
automatically. The service is enabled, its load confirmation marker cleared,
and CPUs 0, 1 and 4 expose C2 states with increasing usage and residency.
At 13:47:48, their C2 residency counters were 18.174, 20.803 and 25.521 seconds
respectively. These are cumulative counters, not a battery savings estimate.

The marker safeguard remains intact. The earlier boot journal recorded a
refusal to load because `/var/lib/zrn-cpuidle/loading` survived a prior reset.
The existing independent 120-second confirmation timer clears successful loads;
`ExecStop` clears the marker during clean shutdown. The observed clean reboot
and timer completion exercised those paths successfully. An unconfirmed forced
reset still deliberately blocks automatic loading; removing that safeguard
would hide a possible module-induced boot hang. A controlled reboot after a
forced reset was not performed by this audit.

## Health findings

- Before the clean reboot, logcat showed an Instagram `DeadSystemException`
  and Panfrost `DRM_IOCTL_PANFROST_CREATE_BO failed (err=12)` messages. These
  observations do not establish that RAM-BOOT restrictions caused the failure.
- At 13:44:35, Launcher3 crashed once while constructing `LauncherRecentsView`.
  Its causal chain ends in `NullPointerException` at
  `SystemUiProxy.getRunningTasks`. No repeating Launcher3 crash or ANR appears
  in the captured filtered errors. Capture was interrupted by a container
  restart and therefore cannot establish a continuous healthy interval.
- At 13:53, SystemUI and system_server were running (Android PIDs 468 and 286).
  A SystemUI service dump returned application state. This proves Binder dump
  responsiveness at that instant, not a complete touch-input acceptance test.
- WhatsApp opened its phone-number screen initially. Later it displayed
  “Download the official WhatsApp to continue” in
  `CustomRegistrationBlockActivity`. Text entry and messaging were not verified.
  Screenshots are in `results/health-whatsapp-start.png` and
  `results/health-field.png`.
- No RAM-BOOT restriction was reverted: the captured evidence does not tie its
  cached-process limit or package policy to the observed failures. Android user
  data, the attestation framework patch, Bluetooth, and ztmetro/browser were
  not changed by this audit.

## Measurements: interrupted, USB connected

Positive current means net charging; negative means net battery discharge.
USB remained online with a 500 mA input limit. Briefly selecting
`inhibit-charge` still produced positive battery current and a charging status;
it was restored to `auto`. These are **net battery readings under USB power**,
not battery-only system draw. Charger status and current also disagreed in
some samples. Sparse one-minute current samples and the charge counter do not
agree closely enough for a precise energy estimate.

| Interval UTC | Elapsed | Net current, sampled mean | Charge counter change | Android CPU, one core | IRQ activity/s |
|---|---:|---:|---:|---:|---:|
| 13:44:31–13:52:36, mixed use and restart | 479.801 s | -15.674 mA | -7 mAh | incomplete; counter reset | 3,156.774 |
| 13:47:47–13:48:47, first keep-awake attempt | 59.919 s | +114.219 mA | 0 mAh | 2.154% | 6,660.778 |
| 13:50:15–13:51:15, retry | 59.988 s | -13.125 mA | +1 mAh | 0.534% | 6,100.584 |
| Frozen control | not measured | — | — | — | — |

Both keep-awake attempts began screen-off and ended screen-on. They are invalid
as screen-off drain trials. IRQ deltas include timer interrupts and IPIs; they
measure interrupt activity, not proven wakeups from suspend. The retry's largest
IRQ rates were arch_timer 3,361.203/s and function-call IPIs 2,119.495/s.

During the first interrupted minute, leading host CPU users were Superscan
7.677%, `sugov:0` 6.843%, `sugov:4` 4.122%, ztmetro 2.737%, and zrnperfd 2.286%.
During the retry, `sugov:0` used 4.801%, `sugov:4` 2.334%, zrnperfd 2.067%,
grim 2.000%, and ztmetro 1.600%. These include concurrent tests, not idle-only
work. No changes were made to the browser or frequency governor from these
confounded samples.

For the mixed-use capture, the surviving sleepd PID 5046 used approximately
0.253% of one core between 13:46:36 and 13:52:36. Network helper PID 5873 used
0.197% between 13:47:36 and 13:52:36. These script processes have generic Python
command names; attribution uses the phone's systemd MainPID readback. Final
service CPU counters were 1.326 CPU seconds for sleepd and 4.790 CPU seconds for
the network service, including its children. They are cumulative service
counters, not A/B deltas.

Raw samples and calculated results:

- `results/health-running.jsonl`
- `results/health-interrupted-awake.jsonl`
- `results/health-interrupted-awake-retry.jsonl`
- `results/health-summary.json`
- `results/health-errors.txt`

Full raw logcat remains on the phone at `/var/tmp/ztos-health/logcat.raw`.
All audit collection services were stopped. Android was left unfrozen
(`cgroup.freeze=0`, `cgroup.events: frozen 0`), with
`keep_awake=com.whatsapp` unchanged and charger behavior `auto`.

## Verification and rerun

The two new policy regression tests passed: a messaging exemption causes no
Android window-state polling or freeze, and idle Android still freezes without
an exemption. Five existing background-sleep tests and three power-ordering
tests passed. Python compilation passed for the modified helper and audit
scripts. The existing dual-identity test has four fixture errors because its
extracted `keep_awake_active` lacks the `keep_awake_packages` dependency; these
are not passing checks and were not changed here.

Use the approved controller to upload the current `tests/health_sample.py` and
`tests/health_ab.py` into `/home/zerone/ztmetro-phase2` on the phone. With a
reserved screen-off window, invoke the latter as root with a fresh output
directory. It measures 1,200 seconds per phase, checks display/freeze state and
container identity, and restores Android on completion or interruption. The
frozen phase intentionally defers notifications for its 20-minute duration.
Keep the controller connection; do not claim battery-only drain while USB is
supplying power. Run `tests/summarize_health.py` on the resulting JSONL files.

For the separate health acceptance run, capture logcat continuously across
30 minutes of normal Android use, then verify SystemUI and WhatsApp input.
Do not count boot crashes, container restarts, registration failures, or a
screen-off A/B run as proof of this requirement.

## Rollback

Restore `/var/tmp/ztos-health/network.before` to
`/usr/local/libexec/ztos-waydroid-network` and restart only
`ztos-waydroid-network.service` to undo the helper changes. Remove
`/etc/systemd/system/zrn-cpuidle.service.d/power.conf` and run
`systemctl daemon-reload` to restore the previous CPU mask on the next boot.
Do not force-unload the active module. The persistent Android setting can be
removed with `settings delete global hide_error_dialogs` inside Android;
doing so permits invisible error dialogs to capture input again.
