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
       starts ztos-gnss.service on the first reader, stops it 30 s after the last
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
