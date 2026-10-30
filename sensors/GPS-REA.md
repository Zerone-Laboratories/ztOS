# GPS on the Galaxy S9 (BCM4773) under ztOS: reverse engineering and plan

**Answer:** stock GNSS is four processes in a chain, and three of them already
run, or can run, unchanged in `/run/android`. ztOS needs one tiny bionic
program (`zrn-gnss-nmea`, written and built here) that loads the stock HAL
`gps.default.so` and serves its NMEA. **No zrn-bbd change is needed.** It
works on the phone (section 7): a fix in about 1 to 2 minutes indoors near a
window, and Android apps in Waydroid get it. Everything still depends on the
sensor hub being up, because GNSS data reaches userspace only through `lhd`.

**Nothing has to be pulled from the phone to run it.** `zrn-android-root`
bind-mounts the whole VENDOR partition, so `/vendor/bin/hw/gpsd` is already in
the chroot. The pull list below is for offline reverse engineering only.

## 1. How stock GNSS works, end to end

```
BCM4773 (GNSS + hub MCU)
  | SPI, Broadcom SSI framing (bcm_gps_spi.c)
/dev/ttyBCM  (misc device, sensors/driver/bcm_gps_spi.c:1239)
  | LHE transport packets: Broadcom RPC, not NMEA
lhd  (/vendor/bin/hw/lhd /vendor/etc/sensor/lhd.conf; owns the chip)
  |-- sensors: /dev/bbd_sensor, /dev/bbd_control <-> kernel SSP
  |-- patch:   /dev/bbd_patch (hub firmware download)
  |-- GNSS:    named pipes <NvStorageDir>/.pipe.gpsd_to_lhd (+ reverse pipe),
  |            carrying "GLL" (ME<->PE RPC) data
gpsd  (/vendor/bin/hw/gpsd -c /vendor/etc/gnss/gps.xml; Broadcom GLL position engine)
  | named pipes /data/vendor/gps/.gps.interface.pipe.to_gpsd / .to_jni,
  | plus an AF_UNIX SOCK_SEQPACKET client to /dev/socket/gps
gps.default.so  (legacy libhardware GPS HAL, soname gps.samsungStarFlagtailandroid.so)
  | GpsCallbacks: location_cb, nmea_cb, ...
android.hardware.gnss@2.0-impl.so  (hw_get_module wrapper)
  | HIDL
vendor.samsung.hardware.gnss@2.0-service  ->  Android framework
```

### Evidence

- **Stock init**, `stock/hub-userspace/etc/init/init.gps.rc`: `lhd` runs as
  `user gps` with `lhd.conf`. `gpsd` is a **separate vendor binary**
  (`/vendor/bin/hw/gpsd -c /vendor/etc/gnss/gps.xml`, `seclabel u:r:gpsd:s0`),
  next to `gllogd`. `post-fs-data` creates `/data/vendor/gps` and deletes
  `gps_started`, `glonass_started`, `beidou_started`, `smd_started` and
  `sv_cno.info`. `vendor.samsung.hardware.gnss@2.0-service.rc` starts the
  HIDL service as `system`.
- **lhd.conf**: `LheBbdPacket=/dev/ttyBCM`, `LheBbdControl=/dev/bbd_control`,
  `LheBbdSensor=/dev/bbd_sensor`, `LhePatch=/dev/bbd_patch`,
  `NvStorageDir=/data/vendor/gps/`, `LhePinMux=P17=M1` (external LNA for GNSS),
  `PortName=tcp:127.0.0.1:300`.
- **lhd strings** (`strings -t x bin/hw/lhd`):
  - `/.pipe.gpsd_to_lhd` (0xc05d) and `/.pipe.chd_to_lhd` (0xa206).
  - mkfifo or named-pipe code from `glw/ipc/IpcInterfaceNamedPipeImpl.cpp`.
  - `GlLhdInterface::ChipData`, `ME2PE seq jump`, `Gnss not opened - dropping
    Gll data`, `GnssAck (%s) received`, and `ESW:READY`, `ESW:NOTREADY`,
    `ESW:CRASHED`.

  So lhd demultiplexes the chip's stream. GNSS ("GLL", ME-to-PE RPC) goes to
  gpsd over the pipes, and sensor traffic goes to bbd. **The protocol is
  Broadcom's binary GLL/LHE RPC, not NMEA.** NMEA exists only above gpsd.
- **gps.xml**: `<hal PortName="lhd" NvStorageDir="/data/vendor/gps/"
  LtoDir="/data/vendor/gps/" ...>`. gpsd reaches the chip through lhd.
  - LTO (Broadcom's assistance-data download) comes from `gllto.glpals.com`,
    and Broadcom's location server is `BCMLS2.glpals.com`.
  - SUPL is on in stock, with cert `/vendor/etc/gnss/gps.cer`.
  - RF settings: `FrqPlan=FRQ_PLAN_26MHZ_2PPM_49_152MHZ_300PPB`,
    `RfType=GL_RF_4755_BRCM_EXT_LNA`.
  - Constellations: Beidou and Galileo are enabled.
- **gps.default.so**, REA/Ghidra (image base 0x100000; file offsets below):
  - `HMI` at 0x3c010: tag `HWMT`, version 1.0. `methods` at 0x3c008 points to
    0x120014.
  - `gps_get_hardware_interface` at 0x2d370 returns the internal interface at
    0x3d608.
  - `gps_proxy_get_gps_interface` at 0x22940 stores that interface, calls
    `LibGpsRegisterMessageHandler()` and returns the proxy `GpsInterface` at
    0x3c1a8.
  - **`GpsInterface.size` = 0x50** (AOSP layout: init, start, stop, cleanup,
    inject_time, inject_location, delete_aiding_data, set_position_mode,
    get_extension). Next comes a 0x70-sized extension table.
  - `proxy__gps_init(GpsCallbacks*)` at 0x22968 keeps the caller's callbacks
    and passes its own table at 0x3c1f8 to the inner interface.
  - `proxy__gps_nmea_cb(long,const char*,int)` calls **`callbacks+0x20`**
    (AOSP `nmea_cb`). It truncates sentences to 0x77 bytes and patches in
    `*00\r\n`.
  - `proxy__gps_location_cb` calls **`callbacks+0x8`** (AOSP `location_cb`),
    except in MS-assisted mode without an SLP location.
  - `proxy__gps_set_position_mode(jjjjjb)` takes a sixth argument, a
    Samsung/Broadcom low-power bool.
  - Strings: `.gps.interface.pipe.to_gpsd`, `.gps.interface.pipe.to_jni`,
    `/dev/socket/gps`, `/data/vendor/gps/libgps.conf`, `GPS Socket Client:
    Waiting for Server ...`, `MessageHandler::OnConnect, gGpsdCrashed`.
  - Needs only liblog, libcutils, libutils, libc++, libc, libm and libdl. It
    does **not** need HIDL or libhardware.
- **android.hardware.gnss@2.0-impl.so** imports `hw_get_module` and uses
  `nmeaCb`. It is a thin HIDL wrapper and is not needed on ztOS.
- **Kernel**, `third_party/vendor-kernel/drivers/sensorhub/brcm/bbdpl/`: the
  `bbd.c:110` devices are `bbd_shmd`, `bbd_sensor`, `bbd_control` and
  `bbd_patch`. **There is no GPS-specific bbd device.** GNSS shares
  `/dev/ttyBCM` (`bcm_gps_spi.c`, ported as `sensors/driver/bcm_gps_spi.c:1239`)
  with the hub RPC, and lhd does the split.

### What lhd does for GPS versus sensors

| | Sensors | GPS |
| --- | --- | --- |
| Chip boot | Downloads the hub patch, powers via `mcu_power`/`GPS_PWR_EN` | Same boot; GNSS is in the same firmware |
| Data out | `bbd_sensor` / `bbd_control` to the kernel SSP | Named pipe to gpsd (GLL RPC) |
| Engine | MCU firmware | Host-side PE inside gpsd |

### Correction to sensors/README.md

Lines 16 to 22 of `sensors/README.md` credit `/vendor/bin/hw/gpsd` with
booting the chip. **It is lhd that boots the chip**, as the README's own table
on line 41 says. gpsd only adds the position engine on top of lhd.

## 2. Plan: least new code

**Recommendation:** run the stock `gpsd` in the same chroot as lhd, and drive
the stock HAL from a 9 KB bionic program that turns `nmea_cb` into a socket.
This keeps Broadcom's whole engine, needs no protocol work, and leaves
zrn-bbd untouched.

Talking to the chip directly would mean reimplementing gpsd's GLL position
engine (raw measurements to PVT), which is not realistic.

```
ztoscsf-sensorhub.service (lhd, existing)
  -> ztos-gnss.service: chroot gpsd -c /data/vendor/gps/gps.xml
                         chroot /data/local/zrn-gnss-nmea
       -> NMEA: tcp 127.0.0.1:10110
                unix /var/lib/zrn-android/data/vendor/gps/ztos-nmea.sock
  -> Linux gpsd (tcp://127.0.0.1:10110)   -> gpsd clients (cgps, gpsmon)
  -> geoclue [network-nmea] nmea-socket   -> GNOME/Qt apps
```

### Blocked on the hub

**All of it is blocked on the sensor hub being up.**

- Without `ESW:READY` from lhd, there is no GNSS firmware and no pipe peer.
- `ztos-gnss --pre` waits up to 60 s for lhd, then fails.
- The unit `Requires=ztoscsf-sensorhub.service`.
- The warm-reboot hub failure (another agent's work) therefore also means no
  GPS.

### Waydroid

Not covered by this work. Waydroid has no GNSS HAL; the open questions below
say what it would take.

## 3. Files added

| File | What |
| --- | --- |
| `sensors/gnss/zrn-gnss-nmea.c` | Bridge source. Declares the bionic ABI itself (no NDK) and supplies its own `_start` that calls `__libc_init`. It `dlopen`s `gps.default.so`, calls `gps_proxy_get_gps_interface`, checks size 0x50, `init` → `set_position_mode(standalone, periodic, 1000 ms)` → `start`, and fans `nmea_cb` out to TCP and Unix clients. `create_thread_cb` uses `pthread_create`. `-d` echoes NMEA to stderr. |
| `sensors/gnss/build.sh` | clang 22 + lld, `--target=aarch64-linux-android29`, link-time stub `libc.so`/`libdl.so`, `PT_INTERP=/system/bin/linker64`, PIE, no TLS, no textrel. |
| `devices/samsung-starlte/overlay/usr/local/lib/ztos-gnss/zrn-gnss-nmea` | The built binary (arm64, 8.8 KB). |
| `devices/samsung-starlte/overlay/usr/local/lib/ztos-gnss/gps.xml` | Stock `gps.xml` with `SuplEnable="false"` (no RIL) and `LogDirectory="/data/vendor/gps/log"`. |
| `devices/samsung-starlte/overlay/usr/local/sbin/ztos-gnss` | `--pre` brings the root up, clears stale state files (as init.gps.rc does), installs the config and bridge into `/var/lib/zrn-android/data`, and waits for lhd. `--run` starts vendor gpsd and then the bridge, and exits if either dies. |
| `devices/samsung-starlte/overlay/etc/systemd/system/ztos-gnss.service` | After/Requires the sensor hub. Restarts on failure, at most 3 times in 5 minutes. |
| `devices/samsung-starlte/overlay/etc/systemd/system/gpsd.service.d/ztos.conf` | Linux gpsd reads `tcp://127.0.0.1:10110`. |
| `devices/samsung-starlte/overlay/etc/geoclue/conf.d/90-ztos-gnss.conf` | geoclue `[network-nmea] nmea-socket=` set to the bridge's Unix socket. |

There are **no zrn-bbd changes**.

## 4. Test procedure, on the phone

1. Confirm the hub is up: `systemctl status ztoscsf-sensorhub` and
   `pgrep -x lhd`. lhd should log `ESW:READY` or
   `LH:MCU Transport Synchronized`.
2. Install the overlay files, then run `systemctl daemon-reload`.
3. Run it by hand first:
   - `/usr/local/sbin/ztos-gnss --pre`
   - `chroot /run/android /vendor/bin/hw/gpsd -c /data/vendor/gps/gps.xml &`
     - **Expect:** `/var/lib/zrn-android/data/vendor/gps/` gains the
       `.pipe.*` and `.gps.interface.pipe.*` FIFOs.
     - **Check:** `ls -la /var/lib/zrn-android/data/vendor/gps/` and
       `ls -la /dev/socket/`.
   - `chroot /run/android /data/local/zrn-gnss-nmea -d`
     - **Expect:** `HAL thread ...` lines, `started; NMEA on ...`, then `$G?GSV`
       or `$GNGSA` lines within seconds.
     - **Expect:** a `fix ... lat=` line within about 1 to 15 minutes outdoors
       (cold start, no LTO).
4. From another shell, run `nc 127.0.0.1 10110` and check that NMEA flows.
5. Run it as a service: `systemctl enable --now ztos-gnss`, then
   `journalctl -u ztos-gnss -f`.
6. Check the Linux side:
   - `pacman -S gpsd` if needed, then `systemctl restart gpsd` and `cgps`.
   - geoclue: `/usr/lib/geoclue-2.0/demos/where-am-i`.
7. **If it fails, collect:**
   - the stderr of both programs;
   - `strace -f -e trace=openat,connect,mkfnod,mknodat -p <gpsd>`;
   - `ls -la` of the gps directory.

## 5. Vendor files to pull (offline reverse engineering only)

Pull these from `/mnt/vendor` on the phone:

- `bin/hw/gpsd`: the engine. Use it to confirm the FIFO names and the
  `/dev/socket/gps` server, its use of properties and DNS, and its uid
  expectations.
- `bin/hw/gllogd`: probably optional; confirm whether gpsd needs it.
- `bin/hw/vendor.samsung.hardware.gnss@2.0-service` and
  `lib64/hw/android.hardware.gnss@2.0-impl-samsung.so`: only if Waydroid
  pass-through is attempted.
- `etc/gnss/*` (any other config) and `etc/selinux/*gps*` (for context only).

## 6. Open questions and risks

1. **`/dev/socket/gps`.**
   - gps.default.so connects to it.
   - Possibly gpsd creates it; possibly Android init did, from a `socket`
     line in an rc file we do not have.
   - `--pre` only creates `/dev/socket`.
   - If the HAL logs `Waiting for Server ...` forever, inspect gpsd.
2. **Properties and DNS.**
   - No property service and no netd run in the chroot.
   - LTO download (`HttpSyncLto`) and the LBS server will likely fail to
     resolve names. bionic resolves DNS through netd's `dnsproxyd`.
   - Fixes still work, but cold starts are slower.
   - Later option: put the downloaded `lto*.dat` file into `LtoDir` from the
     Linux side.
3. **uid.** Stock runs lhd and gpsd as `gps`. Here both run as root, which is
   harmless unless gpsd checks its uid.
4. **The `GpsCallbacks` size** (0x60, AOSP N+) is passed but not verified
   against `proxy__gps_init`'s inner use. All 12 slots are filled, so a
   smaller expected size is safe.
5. **`broadcom::GpsLocation` layout.** It is assumed to be AOSP's in the
   `-d` diagnostic print only. NMEA does not depend on it.
6. **Waydroid.**
   - It needs a GNSS HAL in the container fed from the host.
   - Options: a Waydroid image with a gnss HAL that reads NMEA from a host
     socket, or mock-location injection via a helper app.
   - Not done here.
7. **Power.** gpsd's `LPmode` and `EnableLowPowerPmm` are left at stock.
   **Measure the drain before enabling the service by default**, given
   `zrn-sleepd`.

## 7. Status, 29 October 2026

**GPS works end to end**, on demand only.

```
lhd -> gpsd -> zrn-gnss-nmea (127.0.0.1:10111)
  -> ztos-gnss-gate (127.0.0.1:10110, /run/ztos-gnss/nmea.sock)
       starts ztos-gnss.service on the first reader, stops it 120 s after the last
  -> ztos-waydroid-location (Waydroid), Linux gpsd or geoclue if installed
```

### The crash that hid every fix

- Vendor gpsd died with SIGSEGV about 23 s after start, right after its first
  NMEA burst, every time.
- gdb: `NdkSensorsInfo` in gpsd's sensor thread dereferences the
  `ISensorManager` it gets from `android.frameworks.sensorservice@1.0`. There
  is no hwservicemanager in the chroot, so it is null.
- Fix: `SensorsEnable="false"` in our `gps.xml`. The engine then runs without
  dead reckoning. First run after the fix: a fix within 60 s, 7 to 15
  satellites, accuracy 6 to 24 m.

### Android in Waydroid

- Waydroid has no GNSS HAL, so Android had no `gps` provider at all.
- microG's `network` provider has no Wi-Fi or cell data in the container and
  never produced a location. Google Maps Go asks only `network` (BALANCED).
- `android/waydroid/location/Bridge.java`, run by app_process as root inside
  the container's mount and pid namespaces but in the host network namespace,
  replaces both with test providers and listens for their ProviderRequests.
  It opens the gate only while a request is active (interval up to 5 min)
  and either a Waydroid window is shown (`ztos-waydroid-power-state` is
  `active`) or the request is for a keep_awake app. Without that check, an
  app left open in a hidden window would hold the GPS on for good, because
  Android still sees it as resumed.
- `/usr/local/sbin/ztos-waydroid-location` (ztos-waydroid-location.service)
  starts the bridge whenever Android is up and again after every Android
  restart. It allows MOCK_LOCATION for uid 0.

### Costs

- Each GPS session ends with ztos-gnss's ExecStopPost restarting
  ztoscoresensorframework (see the unit).
- The bridge holds about 27 MB of anonymous memory while Android runs.
- Logs carry no positions; `ZTOS_GNSS_DEBUG=1` prints every fix.

### Every start was a cold one (fixed the same evening)

Measured indoors, near a window: 71 s to a first fix from cold, and 64 s a
minute after a session had ended. Four causes, each seen with strace on gpsd:

| Cause | Evidence | Fix |
|---|---|---|
| gpsd never wrote its store, `gldata.sto` (its directory is on ext4, `/var/lib/zrn-android/data`, and persists) | it opened the file for reading at each start (ENOENT) and never for writing, not even at a clean engine stop | `NvmemWritePeriodMs="20000"` in `gps.xml`: written every 20 s while it runs |
| 22 s waiting for Samsung's RIL | `connect(@VND_Multiclient)` refused once a second for 20 s, then `GPSD:CORE_ON` | ztos-gnss-gate listens on that abstract socket and drains what gpsd sends (two 32-byte requests needing no answer); engine on 4 s after the start |
| no predicted orbits (LTO) | bionic tried `/dev/socket/dnsproxyd` (netd, absent) and found nothing in `/system/etc/hosts`; even with a name gpsd fetched only `rtistatus3.dat`, never `lto2.dat` | ztos-gnss binds a hosts file with `gllto.glpals.com` over the chroot's; `ztos-gnss-lto.timer` fetches `7day/v5/latest/lto2.dat` (182 KB) at most every two days; gpsd reads it at start (37 satellites tracked half a minute in, against 14 to 16) |
| no time or position aiding | `IgnoreJniTime="true"`, nothing injected | zrn-gnss-nmea `-t` (host clock, only when NTP-synchronised) and `-l` (the last fix, uncertainty growing 30 m/s with its age) |

Broadcom's LBS servers (`lbs.glpals.com`, `BCMLS2.glpals.com`) were reachable
for one test and answered; they are left out of the hosts file, as they
trade the phone's data for aiding.

After: 7 to 11 s from gpsd's start to a first fix, after a session ended 1
to 20 minutes before and after a reboot; under 1 s when the gate still held
the engine. Not measured: a cold start outdoors after days off (no store,
old LTO).

### Not dropping it

- ztos-gnss-gate keeps the engine 120 s after its last reader (was 30 s),
  keeps the last fix in `/var/lib/ztos-gnss/last` (root only) and sends each
  new reader `$PZTLF,lat,lon,accuracy,unix_time` first. A reader arriving
  resets the unit's start limit, which a hub lost to SoC sleep used to hit.
- The bridge holds its reader 60 s when the request ends or the window goes
  out of sight, pushing nothing into a possibly frozen Android; the screen
  going off lets go at once. It gives Android the gate's last fix at once
  (its real age, accuracy growing 2 m/s): Maps Go showed a position in the
  second it asked, a live fix +-3 m 7 s later.
- The 17:04 "flapping" was not the shell's policy: Maps Go's window went
  away 10 s after it opened with no Home press logged (Mali page faults
  from an Android render thread just before), and the user went Home 11 s
  after reopening it. Each cut then meant a cold start.
- Navigation: ztMETRO timed the screen out after 60 s whatever Android
  wanted (Waydroid does not pass FLAG_KEEP_SCREEN_ON on), and screen-off
  stops Android. The bridge now touches `ztos-waydroid-keep-screen` while
  Android holds a screen wake lock or, as Maps Go does not navigate
  itself, while moving faster than 3 m/s with a location app in front;
  ztMETRO does not time out while that file is fresh.
- Verified: 25 minutes with Maps Go in front and that file fresh (as a
  navigating app would keep it), no touches: the screen stayed on, one
  gpsd throughout, Android's last location never more than 1 s old. Not
  verified: the bridge seeing a real navigation wake lock or speed (no
  navigation app is installed and the phone did not move).

### Stopping

The HAL client is stopped first (engine stop, HAL cleanup), then gpsd exits
by itself with status 0. Killed together, gpsd had now and then died of
SIGSEGV on its way out; its store was never at stake (it had none). The
sensor framework is still restarted after each session.

### Open

- SoC sleep (zrn-sleepd) with GPS in use kills the hub's lhd and gpsd; the
  gate restarts them on resume. With the screen on (navigation) the phone
  does not sleep. (Fixed on 30 October: section 8.)
- Light, proximity and rotation after a sleep (J7, 30 October, after the
  pin table): measured only after it, since zrn-sleep-init.ko with the pin
  table was already loaded; the "before" case is the 29 October one (the
  hub back in ROM after every sleep, 9 of 9), which took every sensor with
  it. After a 57 s and a 297 s SoC sleep with GPS off, the screen on: the
  accelerometer streamed at once (fresh readings each second), proximity
  published, and a light reading arrived (OpenTone set the screen to
  5000 K); lhd, the hub daemon and ztoscsf-sensord kept their PIDs. The
  room was dark (0 lux), so auto brightness had nothing to react to: it
  held its low level (backlight 15). Nothing in ztOS reads the
  accelerometer or proximity yet (no rotation, no call proximity), only
  zrndevtest's `watch`. And that `watch` has its own fault, unrelated to
  sleep: taking accel or prox off the hub, as sensord does at screen-off,
  asserts the hub's firmware (transport_layer.c line 165, Arg2=5) every
  time, 8 of 8, also with the sensor's delay in the REMOVE payload as
  Samsung's driver sends it; light alone never does (2 of 2). The hub
  recovers by itself in about 18 s. `sensors/gnss/test-hub-screen-off.sh`
  reproduces it.

## 8. Sleep and GNSS, 30 October 2026

### What broke

Two faults, found together with the sleep work (power/suspend/README.md):

- Every SoC sleep cut the BCM4773's enable pin (gpg0-2 in its power-down
  setting: input, pull-down). The chip came back without its patch and the
  first lhd exchange after the wake failed (OnCommunicationError,
  ESW:CRASHED), with or without gpsd running. zrn-sleep-init.ko now holds
  that pin (and gpg0-1) through the sleep; with GPS off the hub survives.
- With gpsd streaming, a sleep through the hub's requests breaks the hub:
  `[SSPBBD] MCU_REQ_RESP timeout` about 5 s after the wake, then `zrn-bbd:
  hub reset (it had stopped answering)`. It recovers by itself, but the
  session is lost. **GNSS holds SoC sleep off (timed-gnss); it does not
  survive a sleep.** Stock never sleeps through a pending host_req, because
  host_req is a wake IRQ there (its EINT wake mask follows enable_irq_wake).
  Since round 2 ours is too (zrn-sleep-init `eint_follow`, power/suspend/
  README.md): with the inhibit bypassed on the bench, a sleep with gpsd
  streaming now comes back on the hub's own request within about 1 s, and
  GNSS goes on without a gap. timed-gnss stays: a sleep of a second is no
  saving.

  gpsd's own wake lock (REA, Ghidra decompiles of /vendor/bin/hw/gpsd, md5
  042ecb3a, symbols from its .gnu_debugdata):
  - `IGpsdInterface::WakeLock(bool, name)` (0x2bb524, ev_19060cc2) logs
    `WakeLock(Acquire|Release,NAME)` and calls IBrcmGpsHalWakeLock's acquire
    or release.
  - `GlGpsdInterface::OnStart` (0x2bff90, ev_860c3ad0) takes the "GPSD" wake
    lock when PlatfConfig's wake-lock mode (offset 0x5d8) is 1 or 2.
  - `AcquireWakelock` (0x2bfa28, ev_10c30a21) takes it if not held;
    `ReleaseWakelock` (0x2bdf30, ev_05b94fc1) only marks a release pending;
    `ProcessWakelockBeforeSelect` (0x2bdf60, ev_b1c6518c) releases it before
    gpsd's select() when a release is pending.
  - `ChipData_GRABSNQ_460790` (0x2bf870, ev_be9b6fab): on the first chip
    data after the HAL alarm fired, it cancels the alarm and, in mode 2,
    takes the wake lock again.
  - Unknown: which mode gps.xml's `WakeLock="geo"` selects (the config
    parser was not decompiled), and what IBrcmGpsHalWakeLock does on this
    system. CONFIG_PM_WAKELOCKS is not built here, and none is wanted: a
    kernel wake lock would be an invisible veto.

### What holds now

- ztos-gnss-gate holds `/run/zrn-sleepd/inhibit/timed-gnss` while the
  engine serves a reader and is starting (90 s) or answering: written as
  `/run/zrn-sleepd/.gnss.tmp`, dated now + 90 s, renamed in, renewed every
  30 s. A dead gate holds nothing for more than 90 s; a dead hub with a
  reader waiting holds nothing after 90 s.
- With the screen off the engine stops 5 s after its last reader (120 s with
  it on), before zrn-sleepd's 15 s; the hold lasts until the unit has
  stopped (gpsd winding down is still gpsd). ztos-gnss.service removes the
  file in ExecStopPost, however the engine ended.
- A reader that keeps it on with the screen off (a keep_awake app sharing a
  live location) gets 10 minutes, then "screen-off location cap": engine
  off, hold gone, no start until the screen is on. A fresh
  `ztos-waydroid-keep-screen` or a `/run/ztos-gnss/keep-NAME` file lifts it.
  Cost of those 10 minutes: about 250 mA.
- All of these timers count on CLOCK_BOOTTIME (the bridge's hold on
  elapsedRealtime), so a sleep cannot stretch them.
- ztos-gnss.service needs the hub with Requisite=, not Requires=: a GNSS
  start never brings the hub up without ztoscsf's reset sequence; with it
  down the start fails at once ("Dependency failed"). `--pre` waits up to
  60 s for lhd and `ready=1`.
- The gate's retry resets the unit's start limit only once the hub has been
  ready for 10 s, so a reader that stays connected is not locked out for
  five minutes, and a flapping hub cannot restart gpsd every 30 s.
- A hub reset (ztoscsf-sensorhub's start, ztoscsf's power retry) holds
  `timed-hub` (45 s) until `hub ready`.

Measured on the phone (screen off, on the charger, so no real sleep):

| Test | Result |
|---|---|
| reader with the hub stopped | `Dependency failed`, 70 ms; no hub start from the GNSS job |
| hub reset | `timed-hub` present until `hub ready`, gone 0.5 s after it |
| a reader, screen off | `timed-gnss` dated +90 s, renewed every 30 s; first fix 7-9 s |
| reader leaves, screen off | engine stopped 6 s later, hold gone with the unit (1.5 s more) |
| gpsd killed 4 times in 3.5 min, reader held | each time the gate restarted it at once; fix back 13-15 s after the kill; no start-limit-hit |

### Joint end-to-end run (30 October, on the charger)

`sensors/gnss/test-sleep-e2e.sh`, one sequence with GPS, sleep, Wi-Fi and
push together; `zrn-sleepd once`, so real SoC sleeps on RTC alarms:

| Phase | Result |
|---|---|
| A: GPS off, 60 s sleep | 57.2 s asleep; Wi-Fi associated 5.4 s after the wake, zrn-pushd logged in 7.3 s after; a reader 0.3 s after the wake got its first fix in 10.7 s; hub PIDs unchanged, 0 crash lines |
| B: a reader held, screen off | `sleep cancelled: inhibit timed-gnss`, no suspend entry; Wi-Fi back in 5.3 s, push in 10.6 s from the unbind; the reader leaving: 'no readers; GPS off' 7 s later, unit stopped and hold gone 1.1 s after that |
| C: reader leaves with the screen on (idle hold), sleep at once | the gate stopped the engine at the screen-off, then a full 57.5 s sleep; 0 crash lines; fix 7.2 s after the next start |
| D: a reader at the 'settle' step | `sleep cancelled: inhibit timed-gnss` 3.3 s later, no suspend; Wi-Fi 5.2 s, push 7 s; fix 12.8 s |
| E: GPS off, 300 s sleep | 297.2 s asleep; Wi-Fi 5.3 s, push 7.1 s; reader 0.4 s after the wake: first fix 9.6 s |
| F: kill -9 the gate while it holds | engine stopped 0.1 s later (the gate unit's ExecStopPost), hold gone 5.8 s later (the new gate's start) |
| G: reader over 10 min, screen off | cap 601.5 s after the reader, engine off, hold gone; screen on: engine back in 1.8 s, fix 10.9 s after the screen came on |

Every handoff logged `grp4=00000000->00000000 chub=0010000f`; no GRP4
latch, so no firmware abort, in this run. The only hub crashes in the run
were the two `watch` asserts above (phases A and E end with a sensors
check), not the sleeps.

### Constellations and SBAS (a 7-minute session, 30 October)

GSV sentences by talker, with the satellites seen:

| Talker | GSV | PRNs (NMEA numbering) | median SNR |
|---|---|---|---|
| GP (GPS) | 1181 | 14 (5-29) | 28 dB-Hz |
| GL (GLONASS) | 788 | 9 (69-85) | 28 |
| GA (Galileo) | 1556 | 14 (104-133, 100 + SV) | 25 |
| BD (BeiDou) | 1895 | 19 (201-237, 200 + SV) | 30 |
| QZ (QZSS) | 389 | 2 | 27 |

GSA came for GP, QZ, BD, GA, GI (NavIC) and GN; NavIC never had a GSV (it
is on L5/S, and the RF front end is L1 only, as on stock: L5 and NavIC stay
out). GGA fix quality: 1 (autonomous) 352 times, 6 (estimated, after gpsd
kills) 12, 0 (none) 26. Never 2: no SBAS correction in use, and no SBAS PRN
(GAGAN 127/128/132, NMEA 40/41/45) in any GSV. SBASDisable stays unset: the
engine decides, and turning it off would change nothing measured.

### Accuracy

zrn-gnss-nmea sends `$PZTFX,accuracy_m,,` for each HAL fix that carries one
(GPS_LOCATION_HAS_ACCURACY); the gate and the bridge prefer it to HDOP x 4.
Over the same session: 363 values, median 6.0 m, against a median of 2.4 m
for HDOP x 4. HDOP x 4 was optimistic by about 2.5 times. First fixes come
with 100 m. No position is in the sentence or the logs.

### Native location

Nothing native reads location today. geoclue and Linux gpsd are not
installed on the phone (`pacman -Q`); only the overlay's
`90-ztos-gnss.conf` and the gpsd drop-in wait for them. A draft interface,
`ial/spec/org.ztos.ial.Location1.xml`, says what the GSI should get from a
device (NMEA endpoint, engine state, fix age); it has no backend yet.
