# Sensors on the Galaxy S9 under mainline: what it takes

Light, proximity, accelerometer, gyroscope, magnetometer and barometer all sit
behind one chip: a Broadcom BCM4773 (GNSS receiver and sensor hub), on SPI.
Nothing is wired to the SoC directly.

## How the stock system talks to it (from the vendor kernel and the phone)

- Kernel, GPL, in `third_party/vendor-kernel/drivers/sensorhub/brcm/`:
  - `bbdpl/bcm_gps_spi.c`: the SPI transport (Broadcom's SSI framing), a tty
    `/dev/ttyBCM`, and the host-request / MCU-request GPIO handshake.
  - `bbdpl/bbd.c`: character devices `/dev/bbd_sensor`, `/dev/bbd_control`,
    `/dev/bbd_patch` (the chip's firmware patch, compiled into the kernel from
    `bbdpl/*/bbd_patch_file_star.h`).
  - `ssp_*.c`: Samsung's sensor protocol (SSP) on top, exposing IIO devices.
- User space, closed: `/vendor/bin/hw/gpsd` (8 MB, Android 10, on the phone's
  VENDOR partition), configured by `/vendor/etc/sensor/lhd.conf`. **It, not
  the kernel, boots the chip** (downloads the patch), and every sensor packet
  the kernel wants to send goes through it: the kernel puts the packet in
  `/dev/bbd_sensor`, the daemon wraps it in Broadcom's RPC and writes it to
  `/dev/ttyBCM`. It powers the chip through
  `/sys/class/sec/sensorhub/mcu_power` and `/sys/class/sec/gps/GPS_PWR_EN/value`.

Stock device tree (`stock/twrp-live.dts`, node `BCM4773@0`): 24 MHz SPI mode 3,
chip select and MCU request/response on one GPIO bank, host request on
another, supply `vdd_mcu_1p8`. The SPI controller is not in the mainline
device tree.

## Where it stands: working

The hub boots on mainline and the light sensor drives the screen brightness.
Light, accelerometer and gyroscope are confirmed to deliver readings; pressure,
proximity and the magnetometer are found by the hub but gave nothing in a
three-second test (proximity only reports changes) and are not looked into yet.

The pieces, bottom up:

| Piece | What it is | Where |
| --- | --- | --- |
| `zrn-bbd.ko` | Broadcom's GPL transport (`bbd`, `bcm_gps_spi`) from the vendor kernel, with a board file in place of the device tree and Samsung's SSP driver | `sensors/driver/`, installed in `/lib/modules/7.1.0-zboot/extra` |
| `lhd` | Broadcom's closed daemon, run unchanged from the phone's VENDOR partition inside `/run/android` (`zrn-android-root`) | `zrn-sensorhub.service` |
| `/dev/zrn_ssp` | the hub's SSP packets, each behind a 2-byte length; a write is one packet to the hub | `zrn_bbd_board.c` |
| `zrnsensord` | speaks SSP, publishes readings, does auto brightness | `ztmetro/sensord/`, `zrnsensord.service` |
| `zrn-ssp.py` | the same protocol as a bench tool: `info`, `watch light:200 accel`, `raw` (stop zrnsensord first, the device has one reader) | `sensors/` |

`zrn-sensorhub` loads the driver behind the same kind of mark as the sound
driver (`/var/lib/zrn-sensorhub/loading`): a load that stops the kernel is not
tried again on the next boot.

### What zrnsensord gives

- `/run/zrn-sensors/light`: `lux cct red green blue clear`; `accel`, `gyro`:
  `x y z` (accelerometer: about 4096 per g); `status`: `auto=`, `screen=`.
- `/run/zrn-sensors/iio:device0/{name,in_illuminance_input}`: the light sensor
  laid out as an IIO device. AutoBrightnessLinux reads it with
  `ALS_IIO_ROOT=/run/zrn-sensors` in its `.env`.
- Auto brightness: lux, smoothed, through AutoBrightnessLinux's curve
  (`log10(lux) / 5`), to the shell's `brightness N`; a new level only when it
  differs by 4 points, ramped a point every 30 ms. The Settings switch sends
  `auto on|off` (remembered in `/var/lib/zrn-sensord/state`).
- Rates: light is asked for every 5 s (`non_batching mode 9 5000` in the hub's
  log), but the firmware samples it every 60 ms and reports whenever the value
  changes, about twice a second indoors; a 5 s batch latency does not change
  that. Files under `/run/zrn-sensors` are rewritten only on a real change
  (5 % lux, at least 2, or 100 K). Screen off, the hub is also sent `0xD2`
  (phone asleep): its status reports stop and its interrupts fell from about
  1.2 to 0.4 a second; `0xD1` when the screen comes back.
- Nothing polls: the hub pushes, the daemon sleeps in `poll()`. zrnperfd
  passes on `screen off|on` and the light sensor is taken off the hub while the
  screen is off (checked: the hub then sends nothing).
- `watch accel|gyro|pressure|prox [MS]` on `/run/zrn-sensord.ctl` streams one
  more sensor for five seconds; ZrnDevTest's sensors page repeats it while open.

### The protocol, in short

The phone opens with 9 bytes: command, length (2), options (2), data (4).
Options: bits 0-1 are 0 read / 1 write / 2 hub-to-phone, the rest a counter.
The hub echoes the options (4 bytes with the length) and then takes the payload
(write) or sends the answer (read). A command with no payload is not echoed.
Add a sensor: command `0xA1`, payload sensor number, delay in ms (4), batch
time (4), options (1). Remove: `0xA2`, sensor number and 4 zero bytes.
What the hub sends unasked are frames of records: `0x37` sensor, reading, 8-byte
time; `0x03` length (2), text of the firmware's log; `0x0A` the hub has started.
Light is sensor 9, 18 bytes: lux (4), colour temperature (4), red green blue
clear (2 each), integration time, gain. Numbers and sizes are in
`third_party/vendor-kernel/drivers/sensorhub/brcm/` (`sensor_list.h`,
`ssp_data.c`); the S9's firmware answers chip id 0x55, revision 19062800.

### What kept the chip silent, in the order found

1. SPI clock and MOSI were swapped: the clock is gph0-0.
2. Chip select polarity was inverted by the spi-gpio platform data.
3. The SoC pulled MCU_RESP down; the stock system has no pull there.
4. The chip's supply, LDO37, is off after boot. It is switched on at load and
   never off again.
5. AP_SLEEP (gpa0-5) must be driven high.
6. Bit-banged SPI was too slow for lhd's three-second patch timeout: the real
   controller at 0x13ac0000 is used, with its clock gates forced and CS_REG 0.
7. The vendor module left a misc device, the `/sys/bbd` kobject and a PM
   notifier behind on unload, which crashed the kernel on the next load; all
   three are released now.

### Cold-boot start: verified

The failing boot had LDO37 on but `gpg0-2` (GPS_PWR_EN) low. `zrn-bbd` now
sets LDO37 on at each probe, holds GPS_PWR_EN low for 100 ms, then drives it
high. The same sequence is exposed at
`/sys/class/sec/sensorhub/reset` for recovery retries. No boot image change is
needed.

Samsung's device tree configures `gpg0-2` as the GPS power output.
`gpg0-4` is an unpulled input assigned to the USB PHY's `phy-port` with
`used_phy_port = <0>`; its sampled DAT bit varies and is not an enable output.
The driver leaves it alone. `gph0-4` is MCU_RESP input with pulls disabled;
`gpa0-5` (AP_SLEEP) is held high; `gpa1-7` and `gpa4-0` remain inputs.

`ztoscsf` no longer has a recovery reboot command. On hub failure it stops
sensord, pulses the driver reset sequence, restarts lhd, and tries twice.
It shows the failure dialog only after both retries fail.

Three normal `systemctl reboot` checks on 2026-10-24 passed. On each boot,
`/sys/class/sec/sensorhub/ready` reached 1 within 28 seconds, sensord was
active by 11 seconds, `gpg0-2` was high (`gpg0 DAT = 0x27`), SPI status was
`0x03000000`, and LDO37 read `ec`. Snapshots:

- `stock-state/arch-hub-normal-reboot-1-20261024.txt`
- `stock-state/arch-hub-normal-reboot-2-20261024.txt`
- `stock-state/arch-hub-normal-reboot-3-20261024.txt`

The earlier failing and TWRP-working snapshots remain in `stock-state/` for
comparison. A synced SysRq-b reboot was attempted; the phone's control endpoint
did not return, so its sensor state could not be verified.

### Not done

User-space failure containment follow-up (2026-10-25): the sensorhub helper now
supervises lhd with a 30-second firmware startup deadline and two-second checks
after readiness. The service does not automatically restart failed firmware;
ztoscsf owns the two recovery attempts and stops both units when those fail.
`python sensors/test-sensorhub-watch.py` exercises the supervisor with mocks,
without touching hardware. The helper, unit, and CSF binary are now deployed. A live failed startup stopped
lhd after 30 seconds and left zero automatic restarts; no lhd survived. The
CSF exhaustion test also verifies stop ordering with mocked hardware commands.
Details and evidence are in `ztmetro/csf/POLISH.md` and `~/swarm/polish-report.md`.

- Auto rotation from the accelerometer, proximity for calls, the compass.
- GNSS: the same chip and daemon, `gpsd` from VENDOR on top of lhd.
- The Settings brightness slider and the action centre tile still move the
  screen while auto brightness is on; the next change in the room overrides them.

## Hub firmware patch: Android 10, not 9 (2026-10-25)

`zrn-bbd` embedded the Android 9 (`p_os`) hub patch while the phone's vendor `lhd` and firmware
are Android 10 (G960NKSU5FVA2). With it the hub asserted in its transport layer
(`Assert=MCU_BCM477X_transport_layer.c line=0165`, then `ESW:CRASHED`) every few minutes to
hours, more under load. `driver/q_os/bbd_patch_file_star.h` is the Android 10 patch from
xxmustafacooTR/exynos-linux-stable (`drivers/sensorhub/brcm/bbdpl/q_os`), now the one built in.
Previous module on the phone: `zrn-bbd.ko.before-qpatch`. That kernel tree's modem_v1 is an
older revision of the same Samsung source we use; nothing to take from it.

The patch alone did not stop the asserts. They came within a second of `ztoscsf-sensord`
sending the AP-state commands 0xd2 (SLEEP, at every screen-off) and 0xd1 (WAKEUP, at its start).
Samsung's ssp driver on this phone never sends either: they belong to early suspend, which its
kernel does not build; it only sends 0xd4/0xd3 (SUSPEND/RESUME) around system suspend.
`ztoscsf-sensord` no longer sends them and only takes its sensors off while the screen is off.
After that: no assert at start and none in four screen off/on cycles (2026-10-25).
