# Galaxy S9 camera driver investigation

Date: 2026-10-25. Target: this repository's Exynos 9810 starlte, not the
Snapdragon S9. No kernel, power-domain or module changes were made.

## Evidence and limits

Resumed phone access succeeded through the mandatory helper. Live `uname -a`
confirms **7.1.0-zboot**, built 2026-10-22 for aarch64. `/proc/config.gz`
confirms `CONFIG_MODULES=y`, `CONFIG_VIDEO=y`, and **MEDIA_SUPPORT disabled**.
There are no `/dev/video*` or `/dev/media*` nodes. `libcamera` is not installed
and has no pkg-config entry. See `evidence/live-inventory.txt`.

The stock vendor partition is mounted at **/mnt/vendor**, not
`/mnt/stockvendor`: `/dev/sda19`, ext4, **ro**. No mount was changed.
`evidence/vendor-inventory.txt` records SHA-256 hashes from that partition.
The initial connection failures are retained as historical evidence only.

Repository evidence identifies the intended kernel as **7.1.0-zboot**:
`power/suspend/secure-handoff-audit.md` records that release from a live query on
2026-10-22, and `sensors/README.md` names its installed module directory.
`build/linux-7.1/Makefile` is version 7.1.0. The source is local at
`build/linux-7.1/`; build inputs and changes are described by the repository root
README and `scripts/build-boot.sh`. There is no `.config` or generated
`include/config/kernel.release` in that source directory at inspection time.

Saved configs `out/ztos-panel-io.config` and `out/ztos-waydroid-compat.config`
both have `CONFIG_MODULES=y` and `# CONFIG_MEDIA_SUPPORT is not set`.
These are saved build configs, not a readback of tonight's running kernel.
`CONFIG_VIDEO=y` in these files is not V4L2 capture support. Enabling a sensor
module alone cannot provide media core, video devices, a receiver or an ISP.

## What already exists

* `stock/twrp-live.dts` contains Samsung's complete camera topology. At line
  11033 is `fimc_is@16290000`; lines 13137 and 13192 identify the 2L3 and 3H1
  modules. The existing `camera/README.md` records rear S5K2L3 at I2C 0x10,
  front S5K3H1 at 0x35, CSIS0/D-PHY, reset/MCLK pins and S2MPB02 rails.
  This describes stock firmware, not devices bound on mainline.
* S9 variants can use IMX345 rear sensors. This unit's stock DT points to 2L3;
  verify the actual module/calibration before choosing tables. The local vendor
  revision contains no path named `imx345` or `pafstat`. Do not substitute an
  unrelated IMX driver based on its name.
* `third_party/vendor-kernel` is Samsung downstream **4.9.118**, revision
  `6a9461f9460ca51d1217e5069a5d5556fcc79e63`. Its sparse working tree omits
  `drivers/media`, but the sources ARE in its Git objects. Read without modifying
  another agent's checkout:

  ```sh
  git -C third_party/vendor-kernel show HEAD:drivers/media/platform/exynos/fimc-is2/Kconfig
  git -C third_party/vendor-kernel ls-tree -r --name-only HEAD drivers/media/platform/exynos/fimc-is2
  ```

  Relevant paths under `drivers/media/platform/exynos/fimc-is2/`:
  `sensor/module_framework/cis/fimc-is-cis-2l3.c`, its setA/setB headers,
  equivalent `fimc-is-cis-3h1*`, `sensor/module_framework/modules/`,
  `fimc-is-device-csi_v4.c`, `hardware/`, `ischain/fimc-is-v6_0_0/`,
  `interface/fimc-is-interface-library.c`, and `vendor/mcd/`.
* `build/linux-7.1/drivers/media/platform/samsung/exynos4-is/` is the old
  Exynos4 camera implementation. It is not an Exynos9810 FIMC-IS2 driver.
  The inspected starlte DTS has reserved camera memory but no matching
  functional Samsung camera graph. Reserved memory is not a capture driver.

## Required capture chain

Sensor rails, reset and MCLK must work before sensor I2C identification. Sensor
mode programming must match MIPI CSI-2 lanes, link frequency and pixel rate.
CSIS and D-PHY then need powered domains, clocks, interrupts and DMA/IOMMU
mapping. A raw V4L2 capture node requires buffer management, frame completion,
formats and a media graph linking sensor and receiver. Samsung's full pipeline
also needs 3AA/ISP, scalers/MCSC and their firmware/library interface. PAF/PDAF
statistics and the lens actuator are needed for phase autofocus, not the first
fixed-focus raw frame. PAFSTAT appears as a preprocessor API concept in this
vendor tree; a standalone driver with that name was not found. OIS and variable
aperture require separate drivers and safe sequencing. Iris/secure capture is
outside this work.

Do not probe gated ISPPRE/ISP registers blindly. The previous raw capture plan
warns that unpowered register access can hang the bus. Inspect Samsung PMUCAL,
clock and regulator sequences first. Shared supplies can affect other phone
hardware. Recovery must be tested with a person present before unattended use.

## Firmware and calibration

`include/fimc-is-binary.h` explicitly names `fimc_is_lib.bin`, optional split
ISP/VRA libraries, `fimc_is_rta.bin`, and firmware variants `fimc_is_fw.bin` /
`fimc_is_fw2.bin`. The 2L3 and 3H1 module drivers request `setfile_2l3.bin` and
`setfile_3h1.bin`. The v6 configuration enables `USE_ONE_BINARY` and
`USE_RTA_BINARY`. The interface maps libraries into kernel virtual memory;
these are not all interchangeable device-side microcode files.

Live read-only inventory found the matching blobs in `/mnt/vendor/firmware/`:

| File | Bytes |
| --- | ---: |
| fimc_is_lib.bin | 3736336 |
| fimc_is_rta_2l3_3h1.bin | 2946528 |
| setfile_2l3.bin | 3677492 |
| setfile_3h1.bin | 3383716 |

Hashes are in `evidence/vendor-inventory.txt`; sizes are in
`evidence/live-inventory.txt`. Android camera provider libraries also exist,
but kernel logs show an unavailable legacy provider executable. These Android
services do not provide a native camera on the current kernel. See
`evidence/kernel-camera.txt`. Calibration was not read or changed.

Look under `firmware/`, `etc/`, `lib*/hw/` and camera subdirectories; retain paths
and hash each matching file. Determine calibration/EEPROM layout using the
matching `vendor/mcd` module code. Do not write calibration or EFS. Firmware,
setfiles, module revision and calibration must match. Android camera HAL shared
libraries are not native Linux V4L2 drivers. Presence of blobs does not prove
that they can run against the current kernel ABI or be redistributed.

## Why building Samsung's stack as modules is not a quick route

`VIDEO_EXYNOS_FIMC_IS2` is a **bool**, depends on `VIDEO_EXYNOS`, and selects
`MEDIA_EXYNOS`, `EXYNOS_FIMC_IS` and `VIDEOBUF2_ION`. Its Makefile aggregates
many pipeline, sensor, memory and firmware objects. `fimc-is-mem.c` includes
Samsung's `linux/exynos_iovmm.h`, `linux/videodev2_exynos_camera.h`, and ION
buffer APIs. The library interface uses architecture-specific memory handling
and Samsung interfaces. These do not become compatible with 7.1 by changing
`bool` to `tristate` or by forcing module version checks.

A port needs modern vb2 DMA allocation, IOMMU support, clocks/power domains,
regulators, device-tree bindings, removed API replacements and an audit of the
binary library's expected ABI and addresses. Some dependencies must be built
into a future kernel; this running config lacks media support. Reusing vendor
4.9 `.ko` files on 7.1 is invalid. An insmod-only experiment is not safe merely
because it compiles: probe can hang, DMA can corrupt memory, and unload may
block shutdown. No modules were built or loaded tonight.

## Mainline, postmarketOS and libcamera

The local mainline source has no Exynos9810 capture pipeline in Samsung's media
directory. The S9+ port README explicitly lists camera as not working. The
upstream Exynos4 FIMC-IS driver supports Exynos4x12, not this ISP generation.

The starlte postmarketOS wiki URL could not be retrieved during this session
(it redirected to `wiki.nura.eco`, which returned Anubis access denied). No current starlte
camera support claim could be verified. Search results for **starqltechn** are
Snapdragon and do not establish Exynos support. A 2025 postmarketOS update says
an S9 device moved to community; that is not evidence that its camera works.
Record this as unknown rather than treating distribution support as camera
support.

libcamera needs kernel camera devices and an appropriate pipeline handler.
Its documented supported ISPs do not list Exynos9810. It cannot create missing
sensor/receiver drivers. Once a raw media graph streams, investigate its simple
pipeline/software ISP or write a dedicated pipeline handler and tuning. Full
Samsung firmware quality would require further ISP and algorithm integration.

Sources checked 2026-10-25:

* https://libcamera.org/faq.html
* https://docs.libcamera.org/master/guides/pipeline-handler.html
* https://android.googlesource.com/kernel/common/+/c2544fb30080aecc/drivers/media/platform/exynos4-is/Kconfig
* https://wiki.postmarketos.org/wiki/Samsung_Galaxy_S9_(samsung-starlte) (retrieval failed)
* https://postmarketos.org/blog/2025/08/03/pmOS-update-2025-07/

## Recommended route and stopping points

1. Live uname/config, DT compatible, media nodes, dmesg, vendor hashes and
   package inventory are complete. Next inspect camera supply/clock ownership
   and calibration layout read-only before any attended bring-up.
2. On a spare device with attended recovery, implement power/clock/reset and
   read the rear sensor ID. Stop on unexpected current, faults or I2C errors.
3. Port the 2L3 mode tables into a mainline sensor subdevice; add CSIS/D-PHY,
   safe domains and DMA capture. Target one low-resolution raw mode, fixed
   focus, no ISP, no flash, no PAF. Validate frame counters and buffer bounds.
4. Add a media graph and basic software demosaic; prove repeated capture and
   idle recovery. Then assess libcamera simple pipeline or a custom handler.
5. Add controls, autofocus/calibration, suspend/reboot recovery and performance
   measurements. Consider Samsung ISP reuse only after a separate ABI audit.

Planning estimates, not promises: inventory a few hours after access returns;
first raw frames several weeks to months for an experienced kernel developer;
usable tuned photography and full ISP support several months or longer.
Hardware documentation and firmware ABI availability are the largest unknowns.
A downstream Android/HAL route may shorten a separate prototype, but replacing
this phone's working mainline kernel is not an acceptable unattended experiment.
