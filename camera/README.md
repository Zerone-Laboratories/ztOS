# Camera on the Galaxy S9: plan for raw capture (2026-10-18)

Goal: frames from the rear sensor over MIPI CSI, without Samsung's ISP
(fimc-is). Full image quality (the ISP, its closed `fimc_is_lib.bin` and
setfiles) is out of scope. Nothing here is built yet: every step below writes
to power-domain, clock or CSIS registers, and on this SoC a write to a block
that is powered off or gated is a silent bus hang that only the keys undo.
Do it with someone at the phone.

**Done on 2026-10-31:** `camera/driver/zrn-cam.c` powers each camera in stock's
order from a module, reads its ID and powers it down, with `zrn-cam-probe`
leaving a mark so a hang is not retried. Rear 0x20C3 rev 0xA5 (S5K2L3), front
0x3081 rev 0xA3, three cycles each. Iris: no answer, `gpio_iris_en` (gpg0-7)
ignores writes from here, so the secure world may own it (not proven).

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
