# The sensor hub and SoC sleep, round 2 — 2026-10-30

**GNSS holds SoC sleep off (ztos-gnss-gate's timed-gnss); it does not
survive a sleep.** Stock never sleeps through a pending host_req, because
host_req is a wake IRQ there: the hub wakes the AP. Ours had host_req
masked in EINT_WAKE, so a sleep with GNSS running slept through the hub's
requests and the hub stopped answering 5 s after the wake (J0c, J0d). This
round makes host_req a wake source and refuses a sleep while the hub talks.

What stock does (REA on stock's Image, stock-syms.txt; the vendor source
agrees):

- `exynos_wkup_irq_set_wake` (0x3e8e20) clears the pin's bit, `1 << (hwirq +
  eint_offset * 2)`, in a global mask on enable_irq_wake(on) and sets it on
  off (0x3e8e74-0x3e8e8c); exynos-powermode.c writes that mask to PMU 0x60c.
  So stock's EINT wake mask follows enable_irq_wake, and bcm_gps_spi
  wake-enables host_req (gpa1-7, "ttyBCM", level-high; IRQ 107 or 109, it
  varies by boot) while lhd
  holds /dev/ttyBCM.
- `ssp_suspend` (0xa83560): AP_SLEEP (gpa0-5) set 0, read back, printk, set
  1 (0xa83594-0xa835c8); uLastResumeState = 0xd4. It sends no command (the
  0xd4 send is commented out in ssp_dev.c too).
- `ssp_resume` (0xa83928): AP_SLEEP 1, then `ssp_send_cmd(0xd3, 0)`
  (0xa839a4-0xa839ac): MSG2SSP_AP_STATUS_RESUME, a bare 9-byte command.
- `bbd_do_transfer` holds comm_mutex from the header until the echo's
  payload is out: one transaction on the link at a time. A length-0
  command is not waited for. The echo wait is 400 ms within 5 s of a
  resume, else 1000 ms (ssp_spi_async).

What ours does now:

| piece | what |
|---|---|
| zrn-sleep-init `eint_follow` (0600, default 0x8000) | EINT bits whose wake follows their IRQ's wake setting; resolved at load (dmesg `eint bit 15 (gpa1-7) follows IRQ 109's wake setting` on 30 Oct). Each sleep, a followed bit whose IRQ is wake-enabled is opened in PMU 0x60c and in the GPIO EINT mask; `eint_effective` shows the word used (0xffe73ffd with lhd up). |
| the refusal | in the finisher, a wake-enabled followed line that reads high (gpa1 DAT bit 7) refuses the sleep with EBUSY: `zrn-system-sleep: refused: host_req high (eint bit 15)`, `hub_refused` counts them. On the line level, not on the pending latch: the IRQ is level-triggered and bcm_spi_suspend disables it with disable_irq_nosync, so a latch with the line low is normal after any served request. That stale latch is write-1-cleared (`hub_pend_cleared`). |
| GRP4 | cleared at each attempt once the line check has passed (`grp4_stale_clear`, default Y, counted in `grp4_stale_clears`); not sticky. `grp4_at_smc` is GRP4 read again right before the SMC; zrn-sleepd's fw-abort test uses it. `clear_grp4` stays, for a person at the bench. |
| handoff line | adds `eint_eff=`, `hub_line=` (-1: nothing followed) and `grp4_at_smc=`. |
| zrn-bbd `ap_sleep_pulse` (0644, default N; Y from /etc/modprobe.d/zrn-bbd.conf), `ap_sleep_pulse_us` (100) | at PM_SUSPEND_PREPARE, AP_SLEEP low for the given time and back high, as ssp_suspend; only while the hub is ready and host_req is a wake IRQ (a hub told the AP sleeps asks to be woken: pulsing with host_req masked is J0c on purpose). `zrn-bbd: AP_SLEEP pulse, read 0`, or `no AP_SLEEP pulse (why)`. |
| zrn-bbd `resume_seq`, `ap_suspends` (/sys/class/sec/sensorhub) | at PM_POST_SUSPEND after a pulse, resume_seq goes up and is sysfs-notified. |
| ztoscsf-sensord | one transaction in flight; on a resume_seq change it queues the bare 0xd3 (`D3 00 00 01 00 00 00 00 00`), behind the open transaction; 400 ms echo timeouts for 5 s after. The only writer on the link, so the kernel sends no hub command. |
| zrn-sleepd | `hub` wake (wake_stat bit 0 with gpa1-7 pending, after key and bt), 5 s awake; `hub-busy` (EBUSY with hub_refused moved), retried in 5 s, not a failure; 3 of either within 120 s of boot time: awake 120 s (`sensor hub keeps the AP awake`). No automatic clear_grp4 any more; clear_grp4 is set back to N at start. |
| bbd_suspend | msleep(20), not mdelay(20). |

Bench results (30 Oct, on the charger, `zrn-sleepd once` through
power/suspend/hub-bench/; the GNSS runs bypass timed-gnss, which only a
bench does):

| run | what | result |
|---|---|---|
| P1 | old sensord: one REMOVE (unwatch accel), screen on | no assert |
| P2 | new sensord: 8 screen-offs each with accel, prox, both watched | 0 asserts in 24 (before: 8 of 8), 0 `no echo`, lhd PID unchanged |
| P3 | new zrn-sleepd, old module, GPS off, 60 s | RTC wake, clear_grp4 N, no hub errors |
| B1 a-d | new modules, GPS off, 3 x 60 s and 300 s | 4 RTC/modem wakes, 0 hub wakes, 0 refusals, 0 MCU_REQ_RESP/Assert/ESW, grp4_at_smc 0, PIDs unchanged; first fix 13.4-17.2 s after a reader came (P3, old module: 20.3 s); bbd_suspend 21, 24, 36 and 520 ms |
| B2a | control, eint_follow=0, GNSS streaming | J0c again: a full 57 s sleep with gpa1-7 pending, then MCU_REQ_RESP twice and two hub resets (recovered by itself) |
| B2b 2, 3 | eint_follow=0x8000, GNSS streaming | `hub` wakes after 0.8 and 1.2 s (wake_stat 0x80000001, gpa1-7); 0 hub errors; GNSS gapless (longest 1.4 s) |
| B3 a-c | pulse on, GPS off, 60 s | RTC wakes, `AP_SLEEP pulse, read 0`, one 0xd3 each, 0 asserts, fix 13.5-14.3 s |
| B3 d-f | pulse on, GNSS streaming | `hub` wakes after 0.7-0.9 s, one 0xd3 each, GNSS gapless; ap_suspends = resume_seq = 0xd3 sent = 6; no `bIsAPSleeping` |
| screen on after B3 | watch accel, prox | first new reading 0.88 s and 0.13 s after screen on |
| B5 | sensors/gnss/test-sleep-e2e.sh A-G (round 1's J1-J3 among them) | 0 crash lines in every phase, the sensor checks after A and E included (they asserted in round 1); Wi-Fi associated 4.6-5.5 s and push logged in 6-8 s after each return; B and D cancelled on timed-gnss; F: inhibit gone 1.1 s after the gate's kill -9; G (run again alone, since run straight after F its reader met the gate restarting): cap 601.3 s after the reader, engine back 1.8 s after screen on |
| B4 x3 | a reader connects at the `suspend` step | the freeze beat the gate: full 57 s sleeps with the reader waiting, GPS on after the wake (fix 12-16 s); no hub reset |

Not seen yet: a `hub-busy` refusal (hub_refused stayed 0: host_req was
low at every check), a stale latch cleared (hub_pend_cleared 0) and a GRP4
stale clear (grp4_stale_clears 0). The 520 ms bbd_suspend is still there
with msleep; its cause is unknown. B2b1 and B2b4 were bench misfires (the
gate rewrote timed-gnss in the bypass's window; then its 600 s screen-off
cap), not results.

The AP_SLEEP pulse is on by default from `/etc/modprobe.d/zrn-bbd.conf`
(`options zrn_bbd ap_sleep_pulse=1`); delete that file to turn it off.
Open: a reader that arrives after zrn-sleepd's re-check waits for the next
wake before GPS starts (B4). Each B4 sleep was 57 s; with the doze and RTC
limits a wait could be up to 300 or 1800 s.

# Pins while the SoC sleeps; sleeps the firmware gives up — 2026-10-30

**The sensor hub died after every SoC sleep because its power pin let go.**
In SYS_SLEEP the TOP pin banks run on their power-down settings (CON_PDN
+0x10, 2 bits a pin: 0 out 0, 1 out 1, 2 input, 3 hold; PUD_PDN +0x14, 4 bits
a pin). gpg0-2 (GPS_PWR_EN, the BCM4773's enable) was at the reset value,
input with a pull-down, so the hub lost power for every sleep that reached
the SoC sequence, aborted ones too, and came back in ROM without its patch
(lhd OnCommunicationError, ESW:CRASHED at the next use; 9 of 9 on 29 Oct).
Stock's DT holds it (pin-con-pdn 3, pud-pdn 0).

zrn-sleep-init now applies a pin table at load (`pdn_pins[]` in platform.c,
one row a pin; later stages only add rows):

| pin | what | CON_PDN | PUD_PDN |
|---|---|---|---|
| gpg0-2 | GPS_PWR_EN, the sensor hub's power | 3 (hold) | 0 |
| gpg0-1 | pm-wrsti, the PMIC's WRSTBI (stock's setting) | 3 (hold) | 0 |

Not in it, on purpose: gpg0-5 (WL_REG_ON; Wi-Fi is unbound for every sleep,
keeping it powered belongs to the WoWLAN work), gpg1-0 (codec reset),
gpg2-0 and gpg3-6 (zrn-bt's own). Mainline pinctrl saves and restores the
PDN registers across suspend, and gpio_restore() touches only CON, DAT, PUD
and DRV, so the write lasts the boot.

Outputs: one dmesg line a pin (`zrn-sleep-init: pdn gpg0-2 conpdn
00002aaa->00002aba pudpdn 01111111->01111011`); read-only `pdn_applied` (bit
8 x bank + pin of PERIC0: gpg0-1 is bit 33, gpg0-2 bit 34) and
`chub_status_before` (PMU 0x14064084 at the SMC, also in the handoff line).
`pdn=0` switches the table off. zrn-sleepd loads the module with insmod,
which does not read modprobe.d, so zrn-sleepd passes it any
`options zrn_sleep_init ...` line from /etc/modprobe.d itself.

Proof, before the module (J0: a live read-modify-write of only those bits,
then bench sleeps, `zrn-sleepd once`, a 45 s NMEA read after the wake):

| run | GNSS during the sleep | pins held | result |
|---|---|---|---|
| J0a | off | gpg0-2 | 60 s RTC sleep; fix 10 s after the start; no hub errors |
| J0b | on | gpg0-2 | firmware gave the sleep up at once (grp4=00000001, EINT gpa1-7 pending); GPS fine |
| J0c | on (clear_grp4=1) | gpg0-2 | 60 s RTC sleep; 5 s after the wake `MCU_REQ_RESP timeout`, `hub reset`; gpsd restarted, fix 7 s later |
| J0d | on | + gph0-4 0/0, gph0-5 1/0 (stock's CHUB rows) | the same `MCU_REQ_RESP timeout`: the CHUB rows do not help; put back |
| J0e | off | gpg0-1, gpg0-2 | 120 s RTC sleep; fix 8 s; no hub errors; PMIC fine |
| J1 (new module, after a reboot) | off | the table | woke by the modem after 16.5 s; fix 14 s; no hub errors |

Before the fix every sleep that reached the SoC killed the hub, GNSS on or
off. With it, an idle hub (lhd up, no gpsd) lives through sleeps. **A hub
streaming GNSS still does not**: the SPI handshake (MCU_REQ gph0-5 / MCU_RESP
gph0-4) times out after the wake and zrn-bbd resets the hub. GNSS holds
SoC sleep off (ztos-gnss-gate's `timed-gnss` while it has readers); it does
not survive a sleep. Stock never sleeps through a pending host_req, because
host_req is a wake IRQ there (round 2, above).

**What GRP4 bit 0 is (J0b).** A sleep with gpsd running found PMU_INTR_GEN
GRP4 = 00000001 at the handoff, came straight back (wake_stat 0x80000001,
smc 0) with gpa1-7 (the hub's host_req) pending in the EINT. So the hub's
data line latches GRP4 bit 0 even though gpa1-7 is masked as a wake source
in EINT_WAKE, and ACPM then gives the sleep up ('upend_wk'). The whole boot
of aborted sleeps on 29 Oct (grp4 was not logged then) may well have been a
hub holding host_req: a chip in ROM after the first sleep fits.

## Sleeps the firmware gives up (zrn-sleepd)

A sleep with smc 0, no wake source (wake_stat & 0x7fffffff, wake_stat2/3 and
every EINT word 0) and under 2 s of boot time asleep, or any sleep with
grp4_before set (grp4_at_smc with the round-2 module), is `fw-abort`: not a
wake, no doze window. It retries after 5 s (it no longer sets clear_grp4:
that stayed Y for the boot and forced later sleeps through live latches;
zrn-sleep-init clears a stale latch itself now); three in a row pause sleeping for the boot
(/run/zrn-sleepd/paused; `zrn-sleepd enable` or a reboot clears it) with the
shell told "Sleep paused". The log line names grp4_before's set bits and the
EINT words.

Also in zrn-sleepd now: the conditions are read again after the settle,
between reading and writing wakeup_count (`cancelled`, 5 s, J3 below); the
failure backoff counts failures in a row (30, 60, 120, 240, 300 s), reset by a
real wake; an alarm due within 2 s (EBUSY from alarmtimer) is `alarm-due`, a
3 s retry; the settle is 1 s when nothing was unbound; at 10 % battery sleeps
last at most 600 s, at 4 % 120 s; a doze window ends PUSH_GRACE_S after the
wake if zrn-pushd's connection lived through the sleep, and at 15 s if
zrn-pushd has no network to try; each sleep logs charge_now in and out; an
hourly summary line (cycles, fw-aborts, wakes by reason, % awake, mAh asleep
and awake); `wakes=` and `woke_at=` in /run/zrn-sleepd/status; the wakeup
sources on a refused or stalled sleep; any inhibit held over an hour.

J3 (live): an inhibit file made during the preparations: `sleep cancelled:
inhibit team-test`, no `PM: suspend entry`, Wi-Fi bound again, the RTC alarm
cleared, entries unchanged.

## The watchdog over the resume window (off)

`wdt_resume_s` (0, off, by default; at most 82 s) arms the cluster-1
watchdog (0x10060000) with reset on and its PMU route (bit 23 of
0x14060408/0x1406040c) open, right after the exit tables return, so a hang
before the watchdog driver's own resume resets the phone. After the resume
(PM_POST_SUSPEND) it is left to the driver if the driver took it back, or
stopped with the route bits put back. wdt-resume.h; host test in
watchdog-test/test-routing.py. Not enabled: no real watchdog-expiry reset
has been seen on this kernel yet, and the first test needs someone there.

## Pin power-down audit (read-only, 30 Oct)

Live CON/DAT/CON_PDN/PUD_PDN of PERIC0, PERIC1, FSYS0, FSYS1 and CHUB,
against stock (sensors/stock-state/pinctrl.txt for PERIC0 and CHUB, the
vendor DT's pin-con-pdn entries otherwise): 31 of 104 pins with a stock
power-down setting differ (everything not in the table is at the reset
value, input with a pull-down). Staged, not applied:

| pins | stock | ours | what to do |
|---|---|---|---|
| gph0-4, gph0-5 (MCU_RESP, MCU_REQ) | 0/0, 1/0 | 2/1 | tried in J0d: no help for a hub streaming GNSS through a sleep; only if an idle hub ever fails again |
| gpg0-5 (WL_REG_ON), gpf2-2 (pcie0_perst) | 3/1, 3 | 2/1 | the WoWLAN and host-wake work, step 0 |
| gpg1-0 (codec reset) | 3/0 | 2/1 | the audio work: would the DSP keep its codec through a sleep |
| gpf0-0, gpf0-1 (FSYS0, function 2) | 3/0 | 2/1 | storage: UFS resumes today; only with a reason |
| gpd0-0 (function 2), gpd0-2 (out, high) | 3/0, 0/0 | 2/1 | look up what they drive first |
| gpg1-7 (out, high) | 0/0 | 2/1 | as above |
| gpg3-4, gpg3-5 | 0/1 | 2/1 | next to zrn-bt's gpg3-6: check with the expander |
| gpp5-0/1, gpp6-0..3, gpc0-0/1, gpg3-0..3, gpf2-0 | 2/0 | 2/1 | pull only: a pull-down against an external pull-up leaks while asleep; a candidate once the sleep floor is measured |
| gpf1-0..5 | 0/0 | 2/1 | unused pins; no change expected |

# Full MIF sleep: stock's suspend path against ours — 2026-10-29

The vendor kernel checkout (third_party/vendor-kernel, LineageOS
exynos9810, 4.9.118) was sparse: only drivers/soc, sensorhub and the DT.
Added drivers/devfreq, drivers/memory, drivers/thermal, drivers/mfd,
drivers/regulator, drivers/soc/samsung, include/soc/samsung, drivers/clk.

What stock's TWRP sleep (build/power-analysis/twrp-stock-sleep.log, MIF down
1) runs before the firmware call, and ours:

| stock | ours (mif-trial.sh) |
|---|---|
| exynos_tmu_suspend: ACPM TMU plugin IPC TMU_IPC_AP_SUSPEND (0x04, rsvd = cp net connected) on channel 12; TMU_IPC_AP_RESUME (0x10) after; TMU_IPC_INIT per zone at boot | plugin 7 copied and attached by load-acpm-tmu.py, never initialised, never told AP_SUSPEND/RESUME |
| devfreq suspend: mif 421000, int 107000, fsys0 73000, cam 590000, intcam 630000, disp 200000, aud 0 (through ACPM DVFS, after DATA_INIT/SET_CONST at boot) | MIF 421 / INT 107 through s9p-acpm FVP; the others untouched |
| s2mps18_pmic_suspend adc_mode 2 | none |
| modem_suspend pda_active 0 (modem left running) | modem powered off |
| etspi suspend SMC, mpsd (TEE), madera, dwc3, sec_ts, hall, fimc_is sensors, iva_ctl | none (their blocks are off or absent in ours) |
| MIF req_Master 0x10 | 0x13 awake (modem bits) |

The TMU row is the lead: the plugin runs on ACPM through the sleep; told
nothing, it may keep polling or act on hardware in the domain going down.
Not proof: trials before 18 Oct hung without the plugin loaded at all.

ACPM SRAM plugin slots (initdata 0x5a0c, 48-byte entries; ACPM address =
offset from 0x02039000): 1 0x1f800+0x800, 2 0x16c00+0x8c00, 4 0xc400+0x2800,
5 0xb400+0x1000, 6 0xec00+0x8000, 7 (TMU, loaded by us) 0xd800+0x1400.
The zero stretch 0xcbc0..0xd800 is inside plugin 4's slot: not a free place
for the sleep log.

Next trial (phone free, someone there for the key reset): send TMU INIT
after loading the plugin, AP_SUSPEND before the sleep, AP_RESUME after
(`s9p-acpm ipc 12 0 0x04 0 0` / `... 0 0x10 0 0`; INIT is `0 0x01 0 0`).

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
