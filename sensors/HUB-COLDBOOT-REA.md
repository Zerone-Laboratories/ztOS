# Sensor hub after a warm reboot: what the stock firmware does

The stock firmware does not run a reset sequence that our driver is missing.
Samsung's kernel never cuts the hub supply and never resets the chip. lhd does
one thing: a 20 ms low pulse on the enable pin (`gpg0-2`, nSTANDBY). Our driver
already does more than that, and it still fails after warm reboots.

The working explanation: **after a warm reboot the BCM4773 is still running,
and nothing we do takes it fully down.** Both our 100 ms enable pulse and
tonight's 3 s LDO37 cut ran while chip select, the SPI clock and AP_SLEEP were
still driven high, and those lines can keep the chip partly powered.
`zrn_bbd_board.c` now parks every line it drives towards the chip low before
it holds the chip in standby. If MCU_RESP is still high afterwards, it repeats
the hold for longer. This is a hypothesis. Only a run on the phone can confirm
it.

## 1. Stock sequence, with evidence

### Regulators

- **LDO37 `vdd_mcu_1p8` is `regulator-always-on`**, with
  `regulator-initial-mode = <3>`
  (`third_party/vendor-kernel/arch/arm64/boot/dts/exynos/exynos9810-starlte_eur_open_26.dts:9048-9057`).
  `set_mcu_power` (`drivers/sensorhub/brcm/ssp_sysfs.c:535-570`) calls
  `regulator_disable`, but an always-on regulator never actually switches off.
  The stock dmesg confirms it: `set_mcu_power: enable vdd_mcu_1p8(0), is
  enabled 1` (`sensors/stock-state/dmesg-hub-boot.txt:1-2`). **Stock never cuts
  the supply**, so the driver keeps refusing to.
- No other regulator, TCXO or clock appears on the BCM4773 node. The `ssp,BCM4773`
  node (dts:7923-7953) has no `clocks` entry and no 32 kHz reference.
- 32 kHz: the S2MPS18 register dumps match between stock and mainline at 0x12
  (`0x57`, `sensors/stock-state/s2mps18-regs*.txt`). If 0x12 is RTC_BUF, as on
  the S2MPS16 and S2MPS17, the 32 kHz output bits are the same on both. The
  vendor tree has no S2MPS18 header, so that register map is **not verified**.
  The PMIC registers that do differ are the interrupt and status registers:
  0x01, 0x02 and 0x04 are pending on mainline, 0x08 is the INT2 mask, and 0x0f
  is the power-on source. Nothing on our side acknowledges the PMIC interrupt.
  That is why **gpa0-1 (the PMIC IRQ pin, dts:387-393) sits low**. It has
  nothing to do with the hub.

### Pins (stock pinctrl, dts lines)

| Line | Pin | Function / pull / power-down | Stock driver action |
|---|---|---|---|
| HOST_REQ | gpa1-7 | EINT, pull-down (1), drv 3 (dts:414) | input, IRQ (bcm_gps_spi.c probe) |
| SHUB_INT | gpa4-0 | EINT, pull-down, drv 3 (dts:423) | input (ssp_dev.c:390-399) |
| AP_SLEEP ("batch-wake-irq") | **gpa0-5** | output, no pull, pdn output0 (dts:404-411) | **output 1 at probe** (ssp_dev.c:376-386); 0 only around suspend |
| MCU_REQ | gph0-5 | output, no pull, pdn output0 (dts:828) | output 0 (bcm_gps_spi.c:1185) |
| MCU_RESP | gph0-4 | input, no pull (dts:817) | input |
| GPS_PWR_EN / nSTANDBY | gpg0-2 | output, pull-down, pdn "previous" (dts:2678-2687) | `gps` node pinctrl `hub_en` (dts:13778); the value is written by lhd |
| SPI | spi@13AC0000, CS gph0-3 GPIO | mode 3, 25 MHz (dts:7905-7953) | — |

### lhd

- `lhd.conf`: `GpioNPowerPath=/sys/class/sec/sensorhub/mcu_power` and
  `GpioNStdbyPath=/sys/class/sec/gps/GPS_PWR_EN/value`. There is no reset path
  and no `GpioNStdbyInitHigh`.
- `BrcmGpsHalGpio::Normal()` at lhd `0x11fa9c` (REA/Ghidra) does this:
  1. If `GpioNStdbyInitHigh` is set, write 1 and wait 50 ms. It is not set
     here.
  2. Read the pin. If it is 1, write 0 and wait 20 ms (the log string says
     "120us").
  3. Write 1 and wait the "Power enable delay".
- Before that comes `mcu_power` 0 then 1, which does nothing on stock. lhd
  then talks to `/dev/bbd_control` (`BBD:PassThru=1`) and sends the patch.
- **The stock kernel adds nothing.** `bcm_spi_probe`
  (`bbdpl/bcm_gps_spi.c:1150-1290`) only claims the GPIOs, and `bbd_mcu_reset`
  (`bbd.c:430`) only queues `RESET_REQ` for lhd.

## 2. What a warm reboot leaves behind

Tonight's failing state, compared with a working boot:

- **MCU_RESP is high while MCU_REQ is low** (gph0 DAT `0x1b`). The chip is
  asserting that it is awake without being asked, so it is not in ROM-idle.
- **SHUB_INT is low.**
- **The SPI status register reads 0.** No transfer has run, or the controller
  is not clocked. The CMU gates were not read, so we cannot tell which.

LDO37 is on and stays on across the reboot, because a warm reboot does not
reset the PMIC. A power-off is the only event that removes the supply. A TWRP
pass leaves gpg0-2 low for as long as TWRP runs, because no lhd runs there.

**Our earlier recovery attempts left lines driven high:**

- chip select (gph0-3, high when inactive);
- the SPI clock (high when idle in mode 3);
- AP_SLEEP (gpa0-5, high).

Whenever a line into an unpowered or standby chip is driven high, current can
flow through the chip's input clamp diodes and partly power it. This is the
most likely reason both earlier attempts (the 100 ms enable pulse and the 3 s
LDO37 cut) failed. **This is inference, not proof.**

## 3. Change (module only): `sensors/driver/zrn_bbd_board.c`

**`hub_power_sequence(hold_ms)`, called at probe and from `sensorhub/reset`, now
does this:**

1. Parks every line we drive towards the chip:
   - MCU_REQ low and AP_SLEEP low;
   - the SPI clock, MOSI and MISO switched from controller function to plain
     inputs (gph0 CON nibbles 0-2 set to 0). Their pull-downs (PUD 0x111) hold
     them low. With `bitbang=1` they are driven low instead;
   - chip select low.
2. Drives GPS_PWR_EN low. With the new `supply_cut=1` parameter (off by
   default) it also switches LDO37 off.
3. Holds that state for `hold_ms`.
4. Switches LDO37 on and waits 20 ms.
5. Restores the lines: AP_SLEEP high, chip select high, then `hw_spi_setup()`
   again. That puts the pins back on the controller and reconfigures the
   controller using the same, already proven CMU writes. `hw_spi_setup` now
   maps its registers only once, so it can be called again.
6. Drives GPS_PWR_EN high and waits 100 ms.
7. Logs `hub standby N ms: MCU_RESP x (was y), SHUB_INT z`. If MCU_RESP is
   still high, it repeats with a longer hold (up to 3 attempts, the hold
   growing by 3x, at least `standby_ms`).

**Hold times:**

- Probe: 100 ms when MCU_RESP starts low, `standby_ms` (default 1000) when it
  starts high.
- The reset file always holds for `standby_ms`.

**What did not change:**

- `mcu_power` 0 is still refused.
- There are no new CMU writes.
- lhd's start in `ztoscsf-sensorhub` is unchanged. Stock lhd needs nothing
  different: its own 20 ms pulse follows ours.
- `ztoscsf` already calls the reset path on failure.

**Build:** `make -C build/linux-7.1 O=out M=$PWD/sensors/driver modules` with
the 14.2 toolchain compiles without warnings. **Before installing, rebuild
against the kernel tree the phone is actually running (the itelasoft
workstation tree).**

## 4. Test procedure

1. Build zrn-bbd against the phone's kernel tree on the workstation, install
   it, and reboot normally once.
2. Warm reboot (`systemctl reboot`) three times. Each time:
   - **Expect** `dmesg | grep 'hub standby'`. A hold of 100 ms means the chip
     came up clean. A hold of 1000 ms means it was left running and the fix
     path was used. The second number in the line should be `MCU_RESP 0`.
   - **Expect** `ready=1` within 30 s and sensord active.
   - **Expect** `hub-snapshot.sh` to read:

     | Register | Address | Value |
     |---|---|---|
     | gph0 CON | 0x13a80000 | `0x00101222` |
     | gph0 DAT | 0x13a80004 | `0x0b` (bit 4 clear) |
     | gpa0 DAT | 0x14050024 | bit 5 set |
     | gpa4 DAT | 0x140500c4 | `0x01` |
     | gpg0 DAT | 0x10430084 | bit 2 set |
     | USI SW_CONF | 0x13a13000 | 2 |
     | SPI | 0x13ac00c8 | 3 |
     | SPI status | 0x13ac0014 | `0x03000000` once lhd has run |
     | CMU | 0x13a02010 | `0x00300000` |
     | LDO37 | — | `ec` |

     gpa0-1 may stay low. That is the PMIC IRQ, and it is harmless.
3. If any boot still fails:
   1. Stop the hub: `systemctl stop ztoscsf-sensord ztoscsf-sensorhub`.
   2. Set a longer hold: `echo 5000 > /sys/module/zrn_bbd/parameters/standby_ms`.
   3. Reset: `echo 1 > /sys/class/sec/sensorhub/reset`.
   4. Read the dmesg line, then start the services again.
   5. If MCU_RESP is still high, try once with the supply cut, now that the
      lines are parked:
      `echo 1 > /sys/module/zrn_bbd/parameters/supply_cut`, then reset again.
4. A SysRq/watchdog reboot can be tested the same way, if it is safe on an
   unattended phone.

## Still uncertain

- **That diode back-powering is the cause.** It is consistent with the
  evidence but not measured.
- **Whether MCU_RESP high after standby means "stale".** In ROM state the
  chip's idle level is unknown. If ROM also holds it high, every boot takes
  about 4 s of escalating holds, which is harmless but slow.
- **Why SPI status read 0.** It could be an unclocked controller rather than a
  missing transfer. Read the CMU gates on the next failure.
- **Which S2MPS18 register enables the 32 kHz output.** Stock and mainline
  match at 0x12, but the register map is not verified.
- **Who drives gpg0-2 high on stock.** The `samsung,exynos54xx-bcm4753` driver
  is not in our partial vendor tree.
