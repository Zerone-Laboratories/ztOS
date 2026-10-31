# Camera on the Galaxy S9: plan for raw capture (2026-10-18)

Goal: frames from the rear sensor over MIPI CSI, without Samsung's ISP
(fimc-is). Full image quality (the ISP, its closed `fimc_is_lib.bin` and
setfiles) is out of scope. The plan below came first; RAW capture is the section above. Every step below writes
to power-domain, clock or CSIS registers, and on this SoC a write to a block
that is powered off or gated is a silent bus hang that only the keys undo.
Do it with someone at the phone.

**Done on 2026-10-31:** `camera/driver/zrn-cam.c` powers each camera in stock's
order from a module, reads its ID and powers it down, with `zrn-cam-probe`
leaving a mark so a hang is not retried. Rear 0x20C3 rev 0xA5 (S5K2L3), front
0x3081 rev 0xA3, three cycles each. Iris: no answer, `gpio_iris_en` (gpg0-7)
ignores writes from here, so the secure world may own it (not proven).

## RAW capture (2026-10-31)

`camera/driver` now takes frames from the rear and front through CSIS with no ISP,
by module, and `/dev/zrn-cam` feeds `ztmetro/camera`. Nothing loads it at boot:

```sh
sudo install -Dm644 camera/driver/90-zrn-cam.rules /etc/udev/rules.d/90-zrn-cam.rules   # once
sudo zrn-cam-probe pmic    # loads the module; the rails stay off
```

- ISPPRE's bus boots at mux 2, divider /16, and the receiver overflows on the first
  frame. The APM's `dvfs_cam` (domain 7, 690000) moves it to mux 0, /1, and the
  receiver then counts about 30 frames a second with no error bits. `dvfs_intcam`
  does not move it. No CMU register is written. The level stays until reboot.
- The DMA writes 16 bits a pixel, 10 bits low, stride 2 x width. Rear 2016x1512 GB/RG,
  front 1632x1224 GR/BG, read from the sensors' colour bars. SysMMU control reads
  0x4; the frames landing where asked, canaries untouched, imply it is bypassed.
- The first frames were taken in a dark room: black level 64, noise growing with gain.
  **No picture of a scene has been checked yet**, nor the 1.4 stops between F1.5 and F2.4.
- Iris: still no answer from the sensor, so no frames.
- A stall of all CPUs (test, 2026-10-31) reset the phone back into ztOS, not TWRP,
  within 48 s, with no one touching it.

## The hardware (stock tree, `stock/twrp-live.dts`)

| Piece | Where |
| --- | --- |
| Rear sensor | Samsung S5K2L3 (`fimc-is-cis-2l3`), I2C `0x10` on hsi2c@10860000 (hsi2c_0) |
| Its module | 13137: `samsung,sensor-module-2l3`, MCLK channel 0 on `gpc1-4`, reset `gpg2-6`, OIS reset `gpg0-6` |
| Around it | AK737x focus `0x0c`, OIS `0x24`, AK7372 aperture `0x4c` (5764, 5773, 5779) |
| Rails | mostly the S2MPB03 at 0x56 (gpm0-0 SCL, gpm1-0 SDA); VDDAF and OIS on the S2MPB02 at 0x59 (gpp6-0 SCL, gpp6-1 SDA) |
| Receiver | CSIS0 in the ISPPRE power domain, under fimc_is@16290000 (11034), D-PHY at 0x16210500 |
| Front | S5K3H1 at `0x35` (6360); iris S5K5F1 runs in secure mode: not attempted |

## Order

1. Rails: the S2MPB02 LDOs for the rear module, through the existing bus
   (safe: the flash already talks to that chip). Register map from
   Samsung's `s2mpb02` regulator driver.
2. MCLK: the sensor needs its clock before it answers on I2C. It comes from
   the camera CMU (ISPPRE): the power domain on through the PMU, then its
   gates; read Samsung's `pmucal` sequence for `pd-isppre` first.
3. Sensor ID over I2C (hsi2c_0 pins, bit-banged like the flash), then its
   register set for one raw mode (from the vendor `fimc-is-cis-2l3.c`).
4. CSIS0 + D-PHY: lane count and data rate from the sensor mode; the CSIS
   writes the frames to memory through its own DMA (the "CSIS WDMA").
5. A V4L2 device, or a simple dump to a file first, and Bayer to RGB in
   software.

## Image quality (2026-10-31)

The whole picture is now made on the GPU, once per sensor frame, from the driver's memory read directly as a
DMA-BUF (R16 on Panfrost, no copy). `ztmetro/camera/raw-source.h`: black 64, radial shading, Malvar-He-Cutler
demosaic, grey-world white balance, colour matrix, sRGB, light S curve; unsharp mask on the screen.

| What | Value | Where from |
| --- | --- | --- |
| Sensor orientation | rear 90, front 270 | observed: `libexynoscamera3.so`, `ExynosCameraSensor2L3Base` and `...IMX320_3H1Base`, field read by `ANDROID_SENSOR_ORIENTATION` in `constructStaticInfo` |
| Black, white | 64, 1023 | observed: same constructors; black also read from dark frames |
| Max analog sensitivity | rear 640, front 800 | observed: same field block |
| Rear colour | forward matrices at 0x1c2944 (A) and 0x1c298c (D65), turned into camera to linear sRGB; rows sum to 1 | observed: data; the constructor picks this set or the one at 0x1c3a94 by an argument not traced, so which is this phone's is inferred |
| Front colour | 330 -80 6 / -72 346 -18 / -12 -119 387, over 256 | observed in `setfile_3h1.bin` at 0x18137; that it means camera to sRGB for daylight is inferred. The 3H1 constructor has no forward matrices |
| WDR on the front's full table | table leaves 0x0216 = 0x0100, and each line alternates a long exposure stock sets | observed: table and `sensor_3h1_cis_set_exposure_time`; the driver writes 0 |
| Full modes | rear 4032x3024 at 2054 Mbps, skew calibration on, PDAF VC 4032x756; front 3264x2448 at 1473 | observed: vendor `fimc-is-device-module-*.c`, `fimc-is-hw-settle-10nm-lpp.h` |

What each frame costs, front camera, app held on the four efficiency cores with `taskset`: 21 % of one core
while focusing, 23 to 28 fps on its own counter.

- Photos stop the preview, power the sensor up at full size, take the fifth frame, develop it with the same shaders,
  read it back and write a PNG on a thread. Rear 12 MP, front 8 MP.
- Focus is contrast only, across the lens range, after exposure settles: `af N` in the driver, in the app's loop.
- Lens shading is a guess (a mild radial gain). Stock's is calibration in the FROM and tables in the setfile, not read.
- The receiver reports a stray DMA bit 12 about once a minute with no visible damage. Its meaning is not known. The
  driver now stops only on 30 errors in 5 s, and logs the first eight.
- Not proven on the rear: a real scene. The phone lay on its back through all of this, so rear AE, AF and colour on
  light are untested; colour bars passed at both sizes.

## Aperture and focus (2026-10-31)

`act-read` and `act-move` in `camera/driver/zrn-cam.c` bring the rear camera up, drive the actuators on bit-banged
gpc0-2/3, and power down. Writes are limited to the registers stock's normal path uses; register 0x03 stores to
the chip's memory and is refused.

- Aperture AK7372 at 0x4c: stock's pulse, 0xa6=0x7b plus end position, about 11 ms, 0xa6=0, back to 511.
- Focus AK737x at 0x0c: IDs f3=0x0e and 03=0x0e, 120 to 200 and back, then stock's landing to 0.
- Register 0x84-85 of both chips changes with a move. After a pulse with the command back at 511, the aperture
  read 0x63-0x6b after F2.4 and 0x85-0x88 after F1.5, and held that through power cycles. Focus read close to
  the commanded position. That it is a hall value is inferred; vendor source does not name it.
- Not proven: the actual f-number. No frame has been captured and nobody has heard it move.
- The aperture is left at F1.5, stock's `APERTURE_CLOSE_VALUE`. OIS is not initialised: star defines
  `USE_OIS_SHIFT_FOR_APERTURE` only for LDU or factory builds, and both are off in `stock.config`.
