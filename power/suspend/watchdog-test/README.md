# Bounded sleep watchdog diagnostic

Temporary diagnostic only; not installed or enabled by the OS installer.
Use only with a suspend interval shorter than the systemd-owned watchdog
timeout. Loading the module maps registers and registers a notifier; it
does not arm a watchdog or change reset routing until CPU0 enters system
suspend with all secondary CPUs offline.

The mainline `s3c2410wdt_suspend()` calls `s3c2410wdt_enable(false)` before
stopping WTCON. For this phone's `samsung,exynos7-wdt` variant, that sets
bit 23 in both PMU `AUTOMATIC_WDT_RESET_DISABLE` (0x14060408) and
`MASK_WDT_RESET_REQUEST` (0x1406040c). The original diagnostic only
re-enabled WTCON, leaving reset blocked. This explains why its timer arm
alone was insufficient; it does not prove the timer clock survives MIF
shutdown or explain DRAM corruption.

The diagnostic now saves and clears those two route bits, verifies their
readback, reloads the existing timer count, and enables reset. A blocked
route aborts CPU-PM entry. Wake and abort restore the original WTCON and
route bits without replacing unrelated PMU bits. The normal watchdog
driver subsequently restores its own state during device resume.

Host regression proof:

```sh
python power/suspend/watchdog-test/test-routing.py
```

This compiles the actual notifier against fake MMIO and checks arming,
wake/abort restoration, preservation of unrelated bits, refusal when reset
remains masked, and no action outside system suspend. A successful module
build or load does not verify automatic reset from a full-MIF hang.

Read-only awake inspection on 2026-10-22 found both PMU words `ff7fffff`
(bit 23 clear), WTCON `00005c39`, and WTDAT `0000fff5`. These awake values
are consistent with systemd's active 30-second watchdog; they do not
establish suspend-time routing or clock continuity.

Two additional AP-only RTC trials on the same boot raised AP/SoC and kernel
suspend success counts to 4, with sensorhub ready=1 and restore errors=0.
The first counter measurement returned WTCNT=65525 both at arm and exit.
The second added a bounded 2 ms awake probe:

```text
count_enter=65525
count_probe=65521
count_exit=65525
control_enter=23609
control_exit=23609
arms=1
route_errors=0
```

The pre-handoff decrement proves the armed timer ticks then. Its increase
back to WTDAT by CPU-PM exit shows a reload somewhere across the handoff;
an unchanged entry/exit value cannot establish that its clock stopped.
Conversely, a pre-handoff tick cannot establish clock continuity through
sleep. No watchdog expiry/reset was tested. Evidence:
`build/power-analysis/watchdog-count-20261022.log`.

## Expiry trial: no reset through AP sleep

The bounded expiry trial at 04:12:45 UTC on 2026-10-22 used the existing
30-second watchdog and a later 45-second RTC alarm, with MIF disabled.
Recovery INFORM2/INFORM3 values were independently read as
`12345678`/`12345674` before entry, and a matching-device TWRP capture and
hot-reboot watcher was running. The watcher was stopped after RTC resume.

The phone woke by RTC (`0x80000002`) after more than 30 seconds of hardware
sleep; its boot ID remained `9d9eb187-1789-45a2-bbe5-8fafc4ff041a`. AP/SoC
sequence counts and kernel suspend successes reached 5. The diagnostic
reported arms=1, route_errors=0, and the same entry/probe/exit timer values
as above. Sensorhub ready=1 and restore errors=0 after wake.

This falsifies relying on this watchdog configuration to enforce its
30-second timeout through AP sleep. It does not distinguish firmware
stopping/reloading the timer from other low-power clock/reset filtering.
Do not use the corrected PMU route alone as authorization to repeat a
known-hanging full-MIF trial. Evidence:
`build/power-analysis/watchdog-expiry-20261022.log`.
