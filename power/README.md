# Power on the Galaxy S9 under mainline

**2026-10-15 update:** see [FIRMWARE-AUDIT.md](FIRMWARE-AUDIT.md) for the
firmware ZIP audit, deployed kernel and timed wake evidence, and current
deep-sleep blockers. Measurements and implementation status below describe
earlier experiments unless the audit updates them.

## Idle drain: why it is high

The deployed kernel registers `zrn_idle_little` and `zrn_idle_big`; per-core
C2 and non-boot-cluster CPD return correctly. A 20-second s2idle trial woke
from the independent MCT alarm with the sensor hub ready. Samsung SICD still
aborts immediately, and its firmware counters have not yet shown SoC or MIF
sequencer activity. The phone offers `s2idle` only; successful screen-off
idle is not yet evidence of complete-system deep sleep.

`drainlog.sh SECONDS [STEP]` logs the fuel gauge to `/var/tmp/drain.log`
(time, %, mV, current now/avg in mA, cable, screen). A baseline needs the
cable out; none has been taken yet.

## Measured: where the idle power goes (2026-10-13)

With the cable out and the screen "off" the phone draws **about 600 mA** (the
fuel gauge's `current_now`; `scripts/dev/power-ab.sh` does it, cumulative steps,
20 one-second readings each). With the cable in, the same changes can be read
as differences of the charge current (noise about +-14 mA).

| step (cumulative) | draw |
| --- | --- |
| baseline, screen off | -594 / -623 mA |
| Wi-Fi radio off (`nmcli radio wifi off`) | -537 |
| sensor hub daemons paused (SIGSTOP lhd, zrnsensord) | -509 |
| modem daemon, perfd, framework paused | no change |
| cpu1-3 offline | no change (-513) |
| display panel put to sleep (`s9p_decon panel_sleep=Y`) | **-356** |

- **The CPU cores are not the drain.** Three cores powered off changed nothing.
  A cpuidle driver that only powers cores down cannot fix the idle draw.
- **The panel is 157 mA** with the screen "off" (the README's 150 mW was a
  guess): `screen_set(false)` only disables DECON; `panel_sleep=N` keeps the
  panel out of sleep mode. Sleep mode needs the panel's own wake sequence
  (the black-spikes issue).
- **Wi-Fi radio 70 mA, sensor hub daemons 28 mA.**
- **~350 mA remain** with all of that off.
- **None of the SoC's power domains matters by itself.** 15 of the 16 are on
  (`scripts/dev/pmu-domains.py`; only the GPU is off, via its genpd), but
  powering each of the unused ones off through the PMU (`pd-ab.py`, CONFIGURATION
  then STATUS, as Samsung's `flexpmu_cal_local_exynos9810.h`) moved nothing.

### What Samsung's kernel does that ours does not

Read from `third_party/vendor-kernel/drivers/soc/samsung/exynos-powermode.c`
and `cal-if/`: three levels, and ours stops at the first.

1. **C2 per core**: the CPU_INFORM hint and wake line (done: zrn-cpuidle,
   zrn-hotplug).
2. **CPD**, cluster power-down, **only for the non-boot cluster** ("power down
   of boot cluster have nothing to gain power consumption, so it is not
   supported"): `big_reset_control(0)` and the cluster sequence, then PSCI
   state `PSCI_CLUSTER_SLEEP`. Putting all four little cores in C2 is not
   something Samsung does; it is what froze this phone.
3. **SICD, System Idle Clock Down**: entered by a boot-cluster core when *every*
   online core is in C2 with enough idle time (`sicd_residency` 10 ms) and the
   PMU's IDLE_IP bits say the blocks are idle: `exynos_prepare_sys_powerdown(SYS_SICD)`
   writes the SICD hint, then PSCI `PSCI_SYSTEM_IDLE`. The always-on firmware
   (ACPM) then runs its sequencers: PLLs off (FSYS0, CPUCL1, CMU_TOP), the memory
   interface down (`soc_sequencer_sicd_down`, `mif_sequencer_sicd_down`).
   This is the step that stops the memory and bus clocks, which nothing in
   mainline does.

SICD's idle list (`exynos-powermode`, `idle-ip` in `stock/twrp-live.dts`) is
every I2C and SPI controller, UFS, the SD controller, USB, both PCIe
controllers, the mailbox, DisplayPort, Bluetooth and **all 15 power domains**.
On this phone the PMU's IDLE_IP status registers read all zero and the masks
all ones (`scripts/dev/idle-ip.py`): nothing reports, and nothing is
required, so the decision is entirely ours.

## Per-core power-down: `idle/zrn-cpuidle.ko`

The stock device tree has one idle state per core, PSCI CPU_SUSPEND `0x10000`
(little: 70/160/2000 us entry/exit/residency, big: 235/220/3500). A device
tree entry alone is not enough and would hang: the 9810's PMU is helped by a
"flexible PMU" controller that must be told around every core power-down.
Samsung's kernel (exynos-powermode.c, cal-if pmucal_cpu.c) writes, per core:

| | before | after |
| --- | --- | --- |
| CPU_INFORM, PMU `0x14060860 + 4*core` | 1 (C2) | 0 |
| GRP2_INTR_BID_ENABLE `0x14070200` | set bit | clear bit |
| pending clear | GRP1 `0x1407010c` if `0x14070108` | GRP2 `0x1407020c` if `0x14070208` |
| PMU_EVENT_INTERRUPT_ENABLE_GRP2 `0x14067f08` | set bit | clear bit |

core = cluster * 4 + cpu, worked out as Samsung's `linear_phycpu()`: the
little A55s report a multithreading-style MPIDR (`0x81000300` is little core 3:
core in Aff1, cluster in Aff2), the big cores a plain one (`0x80000103` is big
core 3). The interrupt bit is 4..7 for little cores, 0..3 for big ones.
Mainline does the same dance for Google's gs101 in `drivers/soc/samsung/exynos-pmu.c`.

The module registers a cpuidle driver per cluster (WFI and C2). It uses a custom finisher
with `cpu_suspend(PSCI_CORE_POWERDOWN, zrn_suspend_finisher)` calling SMC `0xC4000001` with
`cpu_resume`'s physical address, bypassing the upstream `psci_cpu_suspend_enter()` bug where bit 30
being clear caused entrypoint 0 to be passed.

It also registers the Exynos MCT Global Comparator 0 (`0x10040000`, GIC SPI 466, rating 300) as the
hardware oneshot broadcast timer ticking at 26 MHz, so cores whose local arch timers stop during C2
sleep receive timer wakeups via IPIs.

Both `cpu_suspend` and `cpu_resume` addresses are passed in from `/proc/kallsyms`:

    insmod zrn-cpuidle.ko suspend_enter=0x$(awk '$3=="cpu_suspend"{print $1}' /proc/kallsyms) resume=0x$(awk '$3=="cpu_resume"{print $1}' /proc/kallsyms) cpus=0x0c

### Verified Behavior & Topology

- **CPUs 2 and 3 enrolled (`cpus=0x0c`): Verified fully stable.** Cores routinely spend >80% of idle time in C2 power-down with tens of thousands of clean transitions.
- **CPU 0 stays awake (WFI):** Acts as the hardware broadcast timer master on CPU0 for MCT G0 interrupts.
- **CPU 1 stays awake (WFI):** Prevents cluster-level power-down collisions and handles scheduler IPIs.
- **Big cores (CPUs 4-7):** Kept out of C2 cpuidle; CPUs 5, 6, 7 are instead taken completely offline (0 mW) by `zrnperfd` / `zrn-hotplug` during nominal load, and re-booted dynamically via Rapid Boost when demanded.
- **Service:** Installed and enabled at boot via `zrn-cpuidle.service` (`zrn-cpuidle keep`).


## Cores offline and back: `hotplug/zrn-hotplug.ko` (2026-10-13)

Mainline could take a core offline here and not bring it back. Samsung's
kernel, from the dying core itself, writes the hint `CPU_INFORM[core] = 1`
and arms that core's wake line (the three writes in the table above), and
undoes both from the core once it is up again
(`exynos_hotplug_out_callback`, `pmucal_cpu_disable`). Without the hint the
controller leaves the core powered, parked in the firmware, and CPU_ON is
accepted and does nothing (status register still `...0f`).

When matters: once the hint is set and the line armed, the controller cuts
the core's power the first time it waits for an interrupt. Done in an
ordinary (early) hotplug callback, that killed the kernel within 50 ms, twice.
The module does it in the "dying" part of the sequence, on the core with
interrupts off, borrowing the fixed step Xen would use
(`CPUHP_AP_ARM_XEN_STARTING`), and refuses the last core of a cluster early,
where it still can.

Measured: cpu7 off reads `CLUSTER1_CPU3_STATUS` (0x1406201c) = `00880000`,
on again `00100f0f`; eight rounds of cpu5-7 off and on and one of cpu3, no
failure; the three big cores back in 87 ms. zrnperfd takes cpu5-7 offline in
nominal and with the screen off and brings them back for big and rapid
boost (seen: nominal 0-4, load -> 0-7 within 500 ms, idle -> 0-4 after 5 s).
Loaded at boot by ztOSCOR-U (behind its mark).

## What is in place against the drain (2026-10-13)

None of it is the fix (that is the cores powering down, above); it is what
could be done without risking a freeze on a phone nobody is next to.

- `zrn-cpuidle try | keep | never | status` (`rootfs/overlay/usr/local/sbin`)
  loads the idle module for six of the eight cores (`0x7e`) behind a mark.
  Not enabled: to be tried with the charger in and the phone in reach.
- zrnperfd, screen on: programs are kept on the four efficiency cores
  (cpuset) and the big cluster at its lowest clock until the app in front
  pins a core ("dynamic boost": nominal, big at 65 %, rapid). Screen off:
  both clusters and the GPU at their lowest, as before.
- Fewer wake-ups with the screen off: zrn-modemd asks the modem every 120 s
  instead of 15 and does not run `iw`; ztOSCOR-U looks every
  60 s instead of 20; the shell's once-a-second look at the network stops.
- The battery is not run flat: ztOSCOR-U warns at 5 % and shuts the phone
  down at 2 % with no charger in, and `zboot-charge-hold` (an early boot
  unit) keeps a phone that is started below 10 % from booting further: on a
  charger it waits there, clocks at their lowest and the screen dark, until
  10 %; without one it powers off.

## Front LED: `../leds/zrn-led.ko`

The notification LED is three channels of the MAX77705 (registers `0x30..0x36`
at I2C 1-0066). The MFD core already creates a `max77705-rgb` platform device;
mainline's leds-max77705 would bind to it but reads its LEDs from the device
tree. zrn-led binds by name instead and registers `red:status`,
`green:status`, `blue:status` (0..255, the channel current); blinking is done
by the chip (`timer` trigger, period shared by the channels). Installed in
`/lib/modules/7.1.0-zboot/extra`, loaded by udev. `zrn-charge` sets it: red
while charging, green when full or on a cable that does not charge.

Since 2026-10-13: full brightness is 60 % of the chip's (`max_percent`), and
the chip's own ramp makes it breathe (`ramp` beside `brightness`, "UP DOWN"
in ms). Off the charger at 15 % or less it breathes red once a second (up
400 ms, lit 100, down 400, dark 100). The registers are not where mainline's
leds-max77705 puts them: the ramp is 0x36 and the blink times 0x38 (0x35 and
0x37 hold nothing), so until now "blinking" had only been setting a ramp.

## What was done about it (2026-10-13)

- **Panel**: sleeps when blanked (`panel_sleep=Y`), set by ztos-panel-selfmask.service once the self mask is off (see below). Never toggle the parameter while the panel is asleep.
- **Wi-Fi**: zrnperfd switches the radio off after 5 minutes of screen-off on battery, and back on with the screen or the charger.
- **Not done**: SICD/cluster power-down in `zrn-idle` (can freeze the phone; only with someone at it and the charger in), and UFS/USB/PCIe runtime power management (needs an unplugged A/B first).

## The panel: why sleeping it left dark patches, and the fix (2026-10-13)

The S6E3HA8 overlays an image from its own side memory on rows 0-149 and 2810-2959 (the rounded corners): the
"self mask". Samsung's enable is `7A 01 00 00 00 95 0A FA 0B 8F ...`; the bootloader loads the image and enables it.
The side memory is lost in DCS sleep while the mask stays enabled, so after any sleep-in/sleep-out the panel shows
dark patches along the top and bottom (as it did under Plasma). Samsung's kernel reloads the image on each resume.

The fix is Samsung's `self_mask_dis` sequence, once at boot: `F0 5A 5A`, `7A 00`, `F0 A5 A5`. It stays off across
sleeps. [ztos-panel-selfmask](../rootfs/overlay/usr/local/sbin/ztos-panel-selfmask) sends it over /dev/mem and its
service then sets `panel_sleep=Y`. Checked on the phone: clean after repeated sleep cycles, also after a reboot.
The saving (about 157 mA) is still to be measured on battery.

How it was found, for the next panel problem (all in [scripts/dev/dsim.py](../scripts/dev/dsim.py)):

- The stock kernel in `stock/BOOT.img` is uncompressed and relocatable: the panel tables are reached through its
  relocation entries (24 bytes: where, 0x403, value). Sequences are lists of packets: type 6 write, 7 DSC, 10 PPS,
  1 delay, 16 key, 15 a nested sequence. Packets marked dynamic are templates the driver fills per resolution
  (the PPS, scaler, caset, paset bytes in the packet are the 1080x2220 ones; the 1440x2960 rows are in the map tables).
- The panel's PPS (register A2) and every readable register are identical before and after a sleep: nothing readable is lost.
- Register 0F flips 80 -> 40 on sleep-out: the panel reloads its registers from its own memory.
- A DCS read is not harmless here: afterwards the first byte of some registers (CA gamma, B8 IRC, B5, DF) reads 00 and
  the screen is tinted; a sleep cycle brings it back. Long packets are now and then not taken: send keys more than once.
- Brightness: the panel sits at the bootloader's (maximum) level; the shell only darkens pixels. Samsung's set-bl-seq
  (gamma CA, AOR B1, ELVSS B5, ACL B4/55, IRC B8) is what real brightness control needs; its values are per-level tables.

## Samsung's panel sequences (from the stock kernel)

Read out of `stock/BOOT.img`'s panel tables (S6E3HA8, `star_a3_s0_*`). Samsung wraps every panel command in access-key writes
(`9F A5 A5` ... `9F 5A 5A`, `F0 5A 5A` ... `F0 A5 A5`, `FC 5A 5A` ... `FC A5 A5`), which our driver does not, and on every
resume it re-sends the init-seq: 5 ms, DSC on (DSI type 0x07), PPS (type 0x0A, 128 bytes), sleep out (0x11), 10 ms,
scaler `BA`, caset `2A`, paset `2B`, set_area `1A`, TE on `35 00`, err_fg `ED 4C`, ffc `C5`, tsp_hsync `B9`, isc `F6 43`, then
display on (0x29, keyed). Sleep in: display off (keyed), 20 ms, afc-off `E2 00` (keyed), 20 ms, sleep in 0x10, 100 ms.
None of this turned out to be needed for a clean wake (the PPS survives sleep); the patches were the self mask.
[scripts/dev/dsim.py](../scripts/dev/dsim.py) sends these from userspace over /dev/mem, `panel-test.sh` runs one cycle.
