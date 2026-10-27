# Current-install AP sleep verification — 2026-10-22

Kernel #55 (`7.1.0-zboot`) completed two consecutive AP-only RTC suspend
cycles using the installed `zrn-sleepd once 10` path. AP and SoC sequence
counts advanced 0 -> 1 -> 2; kernel suspend successes reached 2. Both wakes
reported `wake_stat=0x80000002`, sensorhub ready stayed 1, and
`last_restore_error=0`. The difference between boottime and monotonic grew
by 6.424 and 6.011 seconds respectively, showing time spent suspended.
The ten-second RTC interval includes sleep preparation, not ten seconds
of hardware sleep.

The temporary corrected watchdog diagnostic armed once per cycle and
reported zero reset-route errors; it was unloaded after each trial.
This verifies suspend-time route setup and normal restoration, not a
watchdog-triggered reset or clock continuity through full MIF shutdown.
USB returned automatically. Wi-Fi was detached/rebound by the daemon;
this does not establish native firmware D3 support. MIF sequencing remained
disabled, with MIF sleep count zero. Full MIF sleep remains unresolved.
Evidence: `build/power-analysis/ap-wake-20261022.log`.

Two subsequent watchdog-counter diagnostic cycles also woke by RTC,
bringing AP/SoC sequence counts and kernel suspend successes to 4. The
watchdog ticks before handoff but its count is reloaded across suspend;
entry/exit counter equality therefore proves neither a stopped clock nor
continuous protection. See [watchdog diagnostics](watchdog-test/README.md).

A fifth AP-only cycle intentionally placed the RTC alarm at 45 seconds,
beyond the watchdog's 30-second timeout. It resumed by RTC without a reset
or boot-ID change, despite verified route unmasking and pre-handoff timer
ticks. This watchdog configuration does not enforce that timeout through
AP sleep and cannot serve as the full-MIF hang recovery guarantee. The
phone recovered normally with sensorhub ready=1 and restore errors=0.

# Full MIF sleep: stock works on this phone, ours does not — 2026-10-18 (later)

Samsung's kernel sleeps with MIF down on this exact phone and boot chain.
TWRP (Samsung 4.9.118 kernel), USB cable unplugged, `echo +15 > wakealarm;
echo mem > /sys/power/state`: flexpmu-dbg `[SLEEP] MIF seq down` 0 -> 1,
`EXYNOS-PM: MIF down. cur_count: 1`, woken by IRQ 277 (s2mps18 RTC), rc=0.
Log: `build/power-analysis/twrp-stock-sleep.log`. So the hardware, sboot,
the EL3 monitor and ACPM are fine; the gap is in what our Linux does or
leaves undone. With the cable in, TWRP refuses suspend (usb_notify, dwc3-otg
wakeup sources): unplug for this test.

Ruled out since (each a hung `mif` trial, stopping at `Calling cpu_pm_suspend`,
old log lines with bit flips afterwards):

* MIF frequency before sleep. Stock logs `devfreq_mif: Suspend_frequency is
  421000`, as we use; 1014 MHz (`freq_info[2]`, not what the driver applies)
  hangs the same way.
* PMIC buck ramp rates. Stock leaves BUCKRAMP/BUCKRAMP2 (0x3f/0x40) at 0x00
  (6 mV/us, from regulator-ramp-delay 6000); mainline keeps the reset
  0x45/0x15. `mif-trial.sh` now writes stock's values; still hangs.
* ACPM's tables. Decoded with REA/Ghidra: sleep_down switches the four MIF
  clock muxes off their PLLs, powers down MIF-side PMU blocks, sets buck
  modes and vddq_mem 450 mV (600 mV on wake), and never writes the DMC: no
  software self-refresh command. Live and CSE3 tables are identical. Row 21
  (op 13) checks PMU 0x14060880 for 0x0BADABCD and otherwise runs a PMIC
  OCP re-init once; ACPM writes the magic itself, stock alike.
* Inputs to the firmware: INFORM 4 (sleep table), PSCI power_state
  0x03000000 (= Samsung's PSCI_SYSTEM_SLEEP), lpm_init (the 19 rows we skip
  are G3D, audio, shortstop, PS_HOLD, DCF), the `mif_always_on` flag (stock
  debugfs shows 1 = MIF sequencing allowed).

Other PMIC differences (TWRP vs ztOS, all registers, via TWRP's
`s2mps18-regs` debugfs and `s9p-acpm read 1`): L2/L31 (SD card) on in TWRP,
L32 always-on in ours, buck output voltages (DVFS state). Files:
`build/power-analysis/pmic-twrp.txt`, `pmic-ztos.txt`. DMC registers awake
in ztOS: `dmc-ztos-awake.json` (TWRP has no /dev/mem).

Stock's suspend does what ours does not, from its log: Trustonic TEE core
switch to CPU0 before hotplug, fingerprint (etspi) suspend SMC, modem
suspend with pda_active 0, every devfreq to its suspend frequency.

The [secure-world handoff audit](secure-handoff-audit.md) traces those
secure calls to the pinned vendor source. The PSCI sleep encoding already
matches stock. Trustonic's logged CPU index is translated to a full MPIDR
before the SMC; fingerprint suspend also has no paired resume SMC in its
callback. These are investigation constraints, not verified MIF fixes.

# Full MIF sleep: what the hang is — 2026-10-18

The RAM loses data while the memory interface is down; the kernel wakes into
corrupted memory and hangs. Evidence, all on kernel #30 with
`mif-trial.sh mif` (modem and audio off, FVP release, MIF 421 / INT 107 MHz):

* Trials 1, 3, 4 and 5 hung; trial 2 was woken after 2 s by an external
  interrupt (wake_stat 0x80000001) before MIF went down, and resumed.
* `zrn-lastlog` (lastlog/, a console copied uncached to 0xff580000, which TWRP
  leaves alone) caught trial 5. The text is readable but has sparse single-bit
  errors throughout, also in lines written long before suspend ("3pace",
  "@M:", "123,680103"). The last line is `Calling cpu_pm_suspend`, just before
  the SMC; nothing after it.
* Control: a hard reset (Vol Down + Power, PMIC) of an awake phone zeroes that
  region instead (61,145 of 65,520 bytes zero); a clean reboot keeps it intact.
  The bit errors therefore come from the sleep, not from the reset.
* Sparse flips in readable text mean the cells kept power but were not
  refreshed: the DRAM was not in self-refresh, or left it, while MIF was down.
* Ruled out while awake (256 MiB pattern held 60 s, 0 bytes changed after each
  step): Samsung's low-power init (40 registers), the ACPM FVP release with
  MIF 421 / INT 107 MHz, and modem power-off.
* Ruled out: the PMIC rails. Decoded with LineageOS's s2mps18-private.h,
  vdd2_mem (BUCK8, 0x2e) is mode 3, always on, and vddq_mem (BUCK9, 0x30)
  mode 2, on with PWREN_MIF, identical to stock. Only L32 (0x69: mainline on,
  stock mode 2) and B6 (0x2a bit 3) differ.
* Our enter/exit tables write no LPDDR4 retention: that step is ACPM's
  (mif_sequencer_sleep_down), whose code matches stock byte for byte. What is
  left is an input that firmware reads and Samsung's kernel sets.

Recovery: after a hang the reset may boot TWRP first, which overwrites
pstore's region, so `/var/lib/systemd/pstore` alone is not enough;
`ztos-pstore-keep.service` keeps each boot's records, and zrn-lastlog's copy
survives TWRP (insmod it again, read /proc/zrn-lastlog-previous).

# Current verification status — 2026-10-16

Latest Wi-Fi resume result: kernel #27 passed two consecutive AP-only
RTC-timed sleep cycles without the temporary PCIe recovery module. PCIe
retrained, brcmfmac rebound, Wi-Fi reconnected, and three router pings passed
after each cycle. The sensor hub stayed ready, sleep entries reached 2, and
`last_restore_error` remained 0. This validates Wi-Fi recovery after AP sleep;
the test explicitly detached brcmfmac before sleep, so firmware D3 entry is
still untested. Full MIF sleep is still unverified. See
[`pcie-recovery/README.md`](pcie-recovery/README.md) for implementation and
logs.

FVP initialization gap fixed and verified while awake: the recovered phone
had `constraints_initialized=0`, `level_count=0`, and empty MIF/INT tables.
`prepare-fvp-policy.py` now parses the supplied CSE3 ECT MINLOCK block
(0x596b8), filters the MIF levels to Samsung's calibrated 421..1794 MHz
range, and generates the exact channel-1 DATA_INIT messages used by
`exynos9810_mif_constraint_parse()`. The eight pairs are
1794000/400000, 1539000/336000, 1352000/336000, 1014000/107000,
845000/107000, 676000/107000, 546000/107000, and 421000/107000 kHz.
The ninth message commits count=8 and boot MIF=1794000 kHz.

`initialize-fvp.py` defaults to inspection and requires explicit --apply,
the exact audited manifest, known firmware pointers, platform dry_run=1,
MIF sequencing disabled, AP marked running, and an uninitialized table.
It sends IPC rather than editing policy SRAM, verifies each table row
before sending the commit, then verifies the committed count and flag.
All nine messages were acknowledged on the live phone; independent
`audit-fvp.py` readback showed initialized=1 and count=8 with every MIF
frequency and INT constraint correct. SSH remained available and hub
ready=1. Evidence: `build/power-analysis/fvp-bootstrap-live.log` and
`fvp-policy-initialized.txt`. No suspend was attempted after this change.
This fixes a prerequisite; full MIF resume is still unverified.

The audit, explicit initialization helper, corrected transport and manifest
are installed in /usr/local/libexec/ztos-power. They are not boot services.
The guarded backend remains dry_run=1, ordinary selection is s2idle, and
MIF sequencing is held disabled. Do not infer sleep safety from successful
awake initialization.

Post-initialization testing passed the awake MIF421/INT107 round trip
(all INT fields verified, 8 MiB checksum unchanged, both frequencies
restored, hub ready=1). The subsequent AP-only RTC cycle
`ztos-initialized-fvp-ap-test.service` also resumed: sleep_ap and soc_count
0->1, entries 0->1, RTC alarm 0->1, wake_stat=0x80000002,
last_error=last_restore_error=0, CPUs 0-4 and hub ready=1. Wall advanced
10.744 seconds while monotonic advanced 1.369 seconds. The post-test clock
audit uniquely matched 1794000 kHz again. Evidence is in
`build/power-analysis/fvp-initialized-*`. The overall unit fails at the
known brcmfmac rebinding step; clean device resume is not claimed. MIF
sequencing remained disabled, and its counter stayed zero. The backend
returned to dry_run=1 with ordinary s2idle selected.

Recovery constraint: the phone's physical buttons are broken and entering
TWRP manually is difficult. Do not repeat full-MIF hang trials that depend
on a button reset. The last trial was recovered and the phone is back on
SSH with sensorhub ready=1, default s2idle, and no experimental backend
loaded. The ordinary 30-second system watchdog has not established recovery
from the MIF hang. Further full-MIF entry needs a verified unattended reset
path; firmware analysis and non-suspend diagnostics can continue meanwhile.

Memory-frequency prerequisite now verified: `audit-mif-frequency.py`
compares all seven live MIF PLL/mux/divider fields against the captured
firmware FV map using Samsung's documented PMS/select/divider masks. The
reboot snapshot uniquely matches 1794000 kHz (level 3). This is hardware
clock evidence, independent of the unsupported mailbox FREQ_GET response.
`audit-samsung-minmax.py` parses the supplied sboot GEN calibration at
0x54548: every MIF ASV row specifies boot=1794 MHz and resume=421 MHz.
Samsung replaces the DT's generic suspend frequency with this calibration
via `cal_dfs_get_resume_freq()`; the DT alone is not the final policy.

An awake, firmware-controlled 421000->1794000 kHz round trip succeeded.
Both states uniquely matched all seven expected clock fields, an 8 MiB
memory checksum remained unchanged, and sensorhub ready stayed 1. Evidence:
`build/power-analysis/mif-frequency-roundtrip.log`, `samsung-minmax.json`,
`mif-frequency-reboot.json` and `mif-frequency-421mhz.json`. No direct PLL
or voltage writes were used. The optional suspend-test arguments
`--mif-suspend-khz 421000 --mif-restore-khz 1794000` now issue these ACPM
requests with restoration on return/failure; the caller must first audit
the pre-test frequency. Neither transition is enabled by default.
The first AP-only RTC test with this frequency transition
(`ztos-fvp-421-ap-test.service`) resumed with entries=1,
last_restore_error=0 and sensorhub ready=1. All seven post-test clock
registers matched the original 1794000 kHz level again. The unit still
reports the separate Wi-Fi rebinding failure; complete device resume is
not claimed from these AP/clock results.
The following full-MIF trial (`ztos-fvp-421-mif-test.service`) did not
restore USB. Correcting the suspend frequency is verified and useful,
but is insufficient to establish MIF retention/resume. Further suspend
trials are held pending a current failure log rather than repeating this
same configuration.

The next calibration prerequisite is INT: all ECT MINMAX rows specify
boot=534 MHz and resume=107 MHz. A combined awake MIF421/INT107 round trip
passed: all 14 INT clock fields matched the firmware's 107 MHz row, the
8 MiB memory checksum was unchanged, and all 14 fields returned to the
534 MHz row (see `build/power-analysis/int-frequency-roundtrip.log`).
The opt-in `--int-suspend-khz 107000 --int-restore-khz 534000` extends the
frequency trial. Suspend lowers MIF first, then INT; return raises INT
before MIF. These flags require explicit MIF frequencies and transport.
The AP-only control (`ztos-fvp-both-ap-test-2.service`) resumed with
entries=1, restore_error=0 and hub ready=1; its final Wi-Fi rebind still
fails independently. The first unit with the same name minus `-2` aborted
before suspend because the hotplug module was absent; it is not a sleep
result. The full-MIF trial `ztos-fvp-both-mif-test.service` again did not
restore USB after the wake interval. Applying both calibrated frequencies
is insufficient to resolve full memory retention/resume. The current
watcher leaves TWRP open for inspection after capture rather than immediately
booting ztOS; a current failure log remains necessary before another trial.

Firmware audit follow-up: the supplied CSE3 `sboot.bin` embeds ACPM at
file offset 0x36c00. `audit-acpm.py` follows the live MIF-down callback
(image 0x8974) and decodes its mode tables; `--boot-image` handles the
uninitialized interface in the embedded boot image. All eight up/down
tables match the live SRAM snapshot byte for byte. Normal sleep contains
32 down and 40 up rows, including firmware-specific operations absent
from the Linux source header. The existing phone firmware therefore
already supplies the MIF shutdown/wake implementation. Do not substitute
the shorter Linux source sequence for the actual firmware implementation.

The stock DT disables DCF, DCPOST and DCRD. Its `pmucal_lpm_init` turns
them off in DCRD/DCPOST/DCF order, but they remain powered on in our boot.
`unused-domains.py` defaults to inspection; `--apply` performs that exact
three-domain shutdown after checking Samsung's SRAM policy, saves original
configuration in /run, and uses bounded status waits with rollback on
failure. `--restore` restores parent first. This awake transition succeeded
and retained sensorhub ready=1. The subsequent AP-only RTC cycle advanced
sleep_ap to 1 with entries=1 and last_restore_error=0; Wi-Fi rebinding still
failed. The following full-MIF cycle (`ztos-domains-mif-test.service`) did
not restore USB. These domain prerequisites alone do not fix MIF resume.

Another missing stock integration is the ACPM FVP devfreq suspend handshake:
`drivers/devfreq/exynos/exynos-devfreq.c:exynos_devfreq_suspend` sends
channel 1 `[devfreq_type, 0, DATA_INIT=5, RELEASE=2]`, then requests the
suspend frequency through devfreq. Resume sends the same message with
word1=1 before restoring the runtime frequency policy. The stock starlte
configuration enables CONFIG_EXYNOS_DVFS_MANAGER; MIF and INT DT nodes both
use ACPM. Our PMIC mailbox driver implements channel 2 and does not implement
this path. This is a confirmed implementation gap, not a proven cause of
the hang. Constraint-table setup, supported frequencies and responses must
be validated before enabling the missing suspend-frequency transition.

The optional `suspend-test.py --acpm-fvp-tool /path/to/s9p-acpm` now sends
those MIF/INT release messages before suspend and reverses them on return
or preparation failure, including possibly applied messages whose ACK was
lost. All four messages were acknowledged in an awake round trip. An
AP-only RTC trial with this path (`ztos-fvp-ap-test.service`) then advanced
sleep_ap 0->1, entries 0->1 and RTC alarm 0->1, with restore_error=0 and
sensorhub ready=1. Both awake messages were acknowledged after resume.
The unit still fails at the separate brcmfmac rebinding step. This does
not establish full MIF sleep or implement suspend-frequency policy.

The diagnostic transport had two unrelated bugs: its DVFS branch fell
through into touch-rail shutdown, and buffer-channel inspection read
absent queue pointers. Both are fixed, with hardware-free dispatch tests
in `test-acpm-tool.py`. The touch rails were restored to 0xec/0xf0 and the
built-in touch driver rebound after software reset. The live request
endpoint that ACKs DVFS commands is channel 0; channel 4 belongs to the
framework and channel 5 is firmware-to-AP MIF notification. FREQ_GET on
channel 0 is acknowledged but returns zero, so the tool now rejects it as
an unavailable frequency rather than presenting it as a measurement.
The loaded DVFS plugin is attached (base 0x16c01, size 35840) and has been
captured as `build/power-analysis/acpm-dvfs-live.bin` for further tracing.
The FVP IPC policy handler is a separate resident plugin (base 0xc401,
ops 0xca1c, IPC entry 0xc5d0), already inside `acpm-live.bin`. The supplied
firmware channel descriptors assign channels 0/1/5 to owner mask 0x10
(this FVP plugin), channel 4 to framework owner mask 1, and channel 2 to
PMIC owner mask 8. Do not confuse framework setup, FVP policy and the
hardware DVFS plugin. The full-MIF follow-up with the acknowledged release
handshake (`ztos-fvp-mif-test.service`) again did not restore USB. The
handshake alone is therefore insufficient; frequency/constraint setup and
the actual hang phase remain unresolved.

The recovery capture at `recovery-20261016T035449Z` contains a later clean
shutdown into recovery, not the suspended kernel's failure. Its SRAM file
was an error string because TWRP lacked /dev/mem. The watcher now creates
the conventional recovery device node if missing and rejects SRAM captures
unless they contain exactly 65536 bytes. A valid freeze capture still
requires entering TWRP directly after the hang, without an intervening
ztOS boot overwriting the continuous console region.

`firmware-state.py` now records the live sequence runner's marker, operation
and index at debug offsets 0x2c/0x38/0x3c, including the ASCII END! sentinel.
These are retained trace fields, not proof of a currently executing step.

AP hardware sleep is verified with RTC wake, stopped monotonic time, CPU
context resume, restored online CPUs and sensor-hub ready=1. Full MIF sleep
has **not** completed a verified suspend/resume cycle.

Important correction: despite the old Linux debug-file name
`mif_always_on`, the live ACPM firmware treats debug SRAM 0x0204f95c as
MIF sequencing enable. **0 disables sequencing; 1 permits it.** Earlier
experiments that wrote 0 did not test full MIF sleep. Disassembly of the
live image shows reads of debug+0x15c copied to runtime+0x6c at image offsets
0x9190 and 0x949a; the conditional branch at 0x93e2 skips sequencing when zero.
The archived sboot master labels map bits 0–4 to CP, AUD, GNSS, VTS, AP.
Live audio/modem shutdown changes are consistent with that mapping.

The gated prototype holds the sequencing enable at zero before Linux
preparation and CPU offlining. Its final saved-context finisher enables MIF
only immediately before SMC. The enable/barrier/SMC/abort-disable sequence
occupies one aligned instruction-cache line, with no intervening function
call or stack access. Resume disables MIF sequencing before platform restore.
This guards the Linux preparation window; it does not prove that the secure
monitor or DRAM retention prerequisites are correct.

The AP-only baseline passed with this guarded backend. The first guarded
full-MIF attempt did not restore USB. The rearmed watchdog did not provide
observable recovery. No fresh crash record has yet been retrieved; the
available pre-test dmesg-ramoops record was an old #17 panic, while the live
kernel was #24. Premature DRAM shutdown remains a hypothesis, not a confirmed
cause of the current failure.

A missing prerequisite has been identified in the source: Samsung's
`pmucal_system_init()` executes the 105-row `pmucal_lpm_init` table before
using the sleep backend. Our backend did not implement this initialization.
The exact source-derived rows are in `init-prerequisites.json`. The
read-only `audit-init.py` compares the 56 always-accessible PMU rows; optional
`--with-memory-clocks` also compares eight MIF clock-handshake / bus DRCG rows.
It skips clocks in potentially powered-off graphics/media domains. Run it
on the recovered phone before deciding which initialization writes are needed.
No blanket initialization or regulator-voltage changes have been made.

All experimental backend, touch and watchdog modules were loaded from /tmp;
none is configured to load automatically at boot. The suspend helper now
requires the guarded module for MIF tests. AP-only validation checks the AP
counter; `--allow-mif-sleep` additionally requires MIF sequence-down.

Latest live initialization audit (after the next TWRP warm boot): 75 rows
were inspected. Little-cluster memory options were 1 rather than Samsung's
2, and all four MIF SYSREG bus DRCG registers were zero. The backend now
applies a source-derived 59-row subset of memory policy, core/MIF clock
settings, regulator wake delays and watchdog LPI reset policy. It changed
37 registers on this phone and verified the defined readback bits. Failed
initialization checks restore all earlier writes in reverse order; the live
rollback audit confirmed no differences from the initial 75-row snapshot.

Full-width all-ones CPU SHORTSTOP writes are excluded because their
reserved-bit readback masks are unknown. MIF DRCG readback is checked against
observed implemented set bits 0x80000e00 and the vendor-required bit12 clear;
reserved zero bits do not falsely fail Samsung's 0xffffefff write. The
post-initialization AP-only RTC cycle resumed with last_restore_error=0,
sensorhub ready=1 and CPUs 0-4 online. Wi-Fi rebinding still reports an error,
so this remains a hardware AP-sleep result, not a fully clean device-resume
validation. Full MIF shutdown still did not restore USB after this fix.

Capture the next failure **while still in TWRP**, before hot-rebooting ztOS.
Our mainline continuous console uses the region TWRP exposes as
`/sys/fs/pstore/pmsg-ramoops-0`. A subsequent reboot replaces that record,
which explains why checking it only after ztOS returned kept showing old
recovery logs. `power/watch-recovery.py` captures this record, the panic
record, recovery dmesg and ACPM SRAM; its optional reboot flag then requests
the required TWRP hot reboot. It accepts only this S9 model/device.

---

# Exynos9810 system-suspend bring-up

`platform.c` implements a separate SYS_SLEEP / SYS_SLEEP_USBL2 backend.
It is experimental. A normal module load validates and maps the tables only:
it does not advertise deep sleep or write hardware registers. The live phone
has also loaded it through `load-diagnostic.sh`, which registers the backend
with `dry_run=1`, then restores the default selection to s2idle. Registration
pins the module until reboot because the kernel suspend API has no module
owner or complete removal of advertised sleep states.

The source table generator copies the eight relevant arrays from Samsung's
Exynos9810 PMUCAL header and embeds the source hash. Normal sleep has 1,103
save-table entries: 646 saved/restored registers and 457 status/debug reads.
USB-L2 has the same entries plus two restore waits. The engine preserves the
stock order, masked writes, conditional power-domain access, and bounded
polling. Save passes never write registers. Saved validity is reset on every
pass so a powered-off domain cannot reuse stale state from an earlier trial.

Host checks:

```sh
python3 power/suspend/generate-tables.py --check
cc -std=c11 -Wall -Wextra -Wno-unused-parameter -Werror \
  -fsanitize=address,undefined power/suspend/sequence.c \
  power/suspend/test-sequence.c -o /tmp/test-sleep-sequence
/tmp/test-sleep-sequence
```

The backend and updated hotplug driver cross-compile against the current
7.1.0-zboot kernel. The mapping-only module test passed on the phone (60
regions). With hardware writes and the firmware call disabled, `pm_test`
device and processor stages passed. The processor stage exercised secondary
CPU shutdown and CPU4 restart, restored CPUs 0-4, and kept sensor-hub ready=1.
Actual SYS_SLEEP now resumes through cpu_resume with successful AP/SoC
sequence-down counters, RTC wake, CPUs 0-4 restored, and sensor-hub ready=1.
MIF sequence-down remains zero: full memory deep sleep is still unverified.
The first real preparation trial froze because generic PM core-test delays
can execute WFE after powerdown is armed. Preparation now runs inside the
saved cpu_suspend context; restoration precedes CPU-PM exit callbacks.
The unsafe test-prepared-core.sh is disabled.

Touch's old application-sleep path left its level-low EINT asserted. A
CPU-PM pin-isolation diagnostic proved the cause; the updated touch driver
stops sensing and clears its event FIFO before sleep, and resumes sensing
on wake. A temporary separately named copy passed actual RTC-timed AP
sleep without pin isolation. The permanent build patch is
kernel/patch-touch-suspend.py. USI selection, reset and clock-request state
also need restoration outside the vendor clock tables.

Early tests were followed by unrequested restarts. Preserving cluster-0's
boot-disabled watchdog state was added; subsequent RTC trials stayed up.
This correlation has not established the original restart cause.

Historical isolation tests with sequencing disabled: audio shutdown changes
firmware MIF master bits from 0x13 to 0x11;
modem shutdown changes 0x13 to 0x12. The archived sboot debug formatter
maps bits 0-4 to CP, AUD, GNSS, VTS, AP respectively. This archived mapping
is consistent with these live changes, though the live bootloader is newer.
CP_CTRL_NS read 0xc02016a2. With sequencing disabled, temporarily clearing the header-defined
SET_SW_MIF_REQ bit12 did not enable MIF sleep and was restored. USB gadget
detach and host-SPI clock stop also failed to enable MIF sleep.

`suspend-test.py --mem-sleep deep --pm-test devices|processors|core` selects
the backend temporarily and restores mem_sleep, the hotplug option, UFS PM
level, and Wi-Fi binding afterward. An actual deep trial requires RTC wake;
MCT is retained only as a fallback for s2idle checks. Full sleep is accepted
only when backend entry/restore and AP/MIF firmware counters confirm it.

The backend supports changing `usbl2=0|1` while system sleep is locked. The live
dry-run module uses normal SYS_SLEEP. Connected full-sleep trials therefore
need explicit USB-gadget detach/rebind or a USB-L2 backend loaded at boot;
they are not battery-only measurements. Do not treat the existence of the
`deep` sysfs option or a passing PM test stage as successful hardware sleep.
