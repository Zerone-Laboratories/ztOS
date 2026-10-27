# Stock secure-world suspend handoff audit

Source: `third_party/vendor-kernel`, commit
`6a9461f9460ca51d1217e5069a5d5556fcc79e63`.
Runtime evidence: `build/power-analysis/twrp-stock-sleep.log`.
This audit identifies differences, not a verified cause of the MIF failure.

## CPU suspend state matches

`drivers/firmware/psci.c:psci_suspend_customized_finisher()` maps
`PSCI_SYSTEM_SLEEP` to `psci_power_state_pack(0, 0, 3)`. The affinity shift
in `include/uapi/linux/psci.h` is 24, giving `0x03000000`, which our
`platform.c` already uses. Stock passes the physical `cpu_resume` address
through `cpu_suspend()`; its finisher contains no separate DRAM
self-refresh command. Changing the state to the generic PSCI power-down
encoding would diverge from stock.

`drivers/devfreq/exynos/exynos9810_bus_mif.c:exynos9810_devfreq_mif_suspend()`
updates a frequency limit when the DVFS manager is disabled. That function
does not issue a DMC self-refresh command either. This does not exclude
other firmware or kernel paths from controlling self-refresh.

## Trustonic core migration differs

The successful stock log records CPU1 going offline, a core-switch request
`0xff000036`, and `CoreSwap ok 1 -> 0` before CPU1 shuts down. Our suspend
backend has no corresponding Trustonic handoff.

The logged `core_id=0` is a software index, not the SMC argument.
`MobiCoreDriver/fastcall.c:mc_exec_core_switch()` replaces that index with
`CPU_IDS[index]` immediately before `_smc()`. In
`MobiCoreDriver/platform.h`, Exynos9810 CPU0 is `0x81000000`, including the
Cortex-A55 MPIDR format bit. A direct call with argument zero would not
reproduce the stock call.

Stock calls `mc_clock_enable()` in `fastcall_work_func()` and runs the
request through its worker. In this pinned Exynos9810 driver,
`MC_CRYPTO_CLOCK_MANAGEMENT` has no definition; `clock.h` provides a no-op
clock helper in that case. Do not infer an extra clock requirement from
the helper's name alone. Secure OS initialization and call CPU still need
to be established before considering a standalone migration request.

Next discriminator: establish whether Trustonic is initialized on the
current mainline boot before considering migration. Stock's
`mc_fc_info()` uses `MC_FC_INFO=0xff000002`; status values are 0 (not
initialized), 1 (bad initialization), 2 (initialized), 3 (halted).
The driver returns status 0 on a failed query too, so a failed query must
not be treated as proof that Trustonic is uninitialized.

Live query on 2026-10-22 at 04:01:22 UTC, kernel `7.1.0-zboot`:

```text
cpu=0 response=ff000002 result=0 state=0 extended=0
```

`tee-info/zrn_tee_info.c` issued the stock information call once on CPU0
using `work_on_cpu()`. The successful result (`0`) and status (`0`) show
Trustonic was not initialized on this boot, rather than a query failure
being converted to status zero. The module was then unloaded. No secure
initialization, migration, fingerprint command, suspend, or reboot occurred.
This removes an initialized TEE context on a secondary CPU as the current
migration problem; it does not prove that secure-world initialization has
no bearing on DRAM policy.

## Fingerprint secure suspend differs

The successful stock log records `etspi_pm_suspend: suspend smc ret = 0`.
`drivers/fingerprint/et5xx-spi.c:etspi_pm_suspend()` selects
`MC_FC_FP_PM_SUSPEND=0x83000021` with zero arguments when the data-ready IRQ
flag is clear. Its other branch uses `0x83000028` to retain chip select
high. Definitions are in `drivers/fingerprint/fingerprint.h`.

The resume callback only restarts the debug timer; it does not call the
defined `MC_FC_FP_PM_RESUME` command. Do not invent a paired resume SMC.
Our backend currently issues neither fingerprint command. The evidence
does not establish that either command controls DRAM retention.

## AP memory-request evidence

Read-only ACPM inspection on the same boot after the Trustonic query:
`mif_requests=19` (`0x13`: CP, AUD, AP), sequencing enabled, and zero AP,
SoC, and MIF sleep entries. Sensor hub ready remained 1. Sequencing enabled
while awake is the boot policy, not evidence that MIF is shutting down.

Three saved successful AP-only trial logs report
`mif_requests_before=16` (`0x10`, AP) at the final finisher:
`fvp-initialized-ap-test.log`, `wifi-resume-recovered.log`, and
`wifi-native-resume.log` under `build/power-analysis`. These were measured
before the SMC with the MIF gate held closed. They do not establish request
state in the failed full-MIF trials, but do show that AP request ownership
survived CPU offlining in those controls. An extra temporary AP request is
not justified merely by corrupted text mentioning CPU offlining.

`platform.c` now logs the requested MIF mode, final request byte, and gate
before enabling sequencing, so a retained failure console can distinguish
these cases. The aligned gate/barrier/SMC interval remains unchanged.
Corruption of older log lines does not establish when DRAM lost refresh.

The saved `dmc-twrp-raw.txt` contains address/offset pairs without register
values. It is not a stock register snapshot and must not be compared with
`dmc-ztos-awake.json` as if it were one.

## Test boundary

Do not repeat the failed full-MIF sequence merely to test these differences.
The current Linux watchdog has not demonstrated recovery from that hang.
First resolve secure-world state and establish a recoverable test path.
Keep normal AP-only sleep and the MIF gate policy unchanged meanwhile.
