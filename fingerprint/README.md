# Rear fingerprint sensor (Galaxy S9, starlte)

Status 2026-11-21: investigation done; both kernel modules port and build
against the phone's running kernel. **Nothing has been loaded, probed or
enrolled on the phone.** The test plan below is waiting for the owner's approval.

## Route

Run Samsung's stock fingerprint stack unchanged: the HAL, its libraries and
the Kinibi trustlet. ztOS supplies two kernel modules, replacing the stock
`gud` MobiCore driver and `et5xx-spi`.

Driving the sensor directly from Linux is not viable. SPI1 is in
`secure-mode`, and the image capture and matching code live only inside the
TA (`ffffffff00000000000000000000002e.tlbin`).

```
zrn-fingerprintd (ztOS, D-Bus net.ztos.Fingerprint1)
  | HIDL over /dev/hwbinder, inside chroot /run/android
  v
vendor.samsung.hardware.biometrics.fingerprint@3.0-service
  (ISehBiometricsFingerprint, extends android...fingerprint@2.1::IBiometricsFingerprint)
  -> fingerprint.default.so -> libbauthserver.so (egisFp* path)
       |-- /dev/esfp0 ioctls ----------------> zrn-etspi.ko: power, reset, DRDY IRQ, EL3 CS/PM SiP calls
       '-- libbauthtzcommon -> libMcClient -> /dev/mobicore-user
                                                 zrn-mobicore.ko <- mcDriverDaemon (/dev/mobicore admin)
                                                    | SMC fastcalls + MCI/NQ/MCP buffers, notify IRQ GIC SPI 233
                                                    v
                                                 Kinibi t-base-EXYNOS64-Android-400C
                                                    '- fingerprint TA ...002e.tlbin (Egistec ET510 + CrossMatch matcher)
                                                         '- secure SPI driver -> SPI1 (10450000) -> ET510
```

## Hardware facts

The source is the stock G960N `boot.img` DTB; board revisions rev16 to rev26
are identical.

| Item | Value |
|---|---|
| Sensor | Egistec **ET510**, `compatible = "etspi,et5xx"`, `etspi-chipid = "ET510"`, orient 0 |
| Bus | **SPI1**, `spi@10450000`, CS 0, max 13 MHz, **`secure-mode`**, IRQ SPI 0x18e |
| SPI pins | gpp0-0 CLK, gpp0-1 MOSI, gpp0-2 MISO, gpp0-3 CS; function 2, pull-down, drive 2 |
| DRDY (finger detect) | **gpa1-3** (`btp-irq`) |
| LDO (power) | **gpm43-0** (`btp-ldo`) |
| SLEEP/RESETn | **PCAL6524 P0.6** (I2C 0x22). On ztOS this is the bit-banged bus i2c-gpio 96, which zrn-bt owns; BT_EN is P0.7 on the same chip |
| TEE | Kinibi (Trustonic) t-base 400C, mcRegistry in `/vendor/app/mcRegistry` |
| TEE notify IRQ | stock `tee { compatible = "samsung,exynos-tee"; interrupts = <0 0xe9 0>; }`, i.e. GIC SPI 233 |
| EL3 SiP calls | `0x83000021` PM_SUSPEND, `0x83000027` CS_SET, `0x83000028` PM_SUSPEND_CS_HIGH |

**Is SPI1 secure-locked? The source answers this (P1 skipped).** In the
secure build, `et5xx-spi` never touches SPI1's registers. It only does these
things:

- enables `fp-spi-pclk` and `fp-spi-sclk`, with `clk_set_rate(sclk, speed*4)`;
- muxes the pins;
- calls EL3 for chip select and suspend.

The TA opens the bus with `sec_tzspi_open` (in the TA's strings). A
normal-world SPI driver must therefore never bind to SPI1. That is why
`dts/starlte-fingerprint.dtsi` has no `&spi_1` node.

## ztOS now (read 2026-11-21)

- **Kernel:** 7.1.0-zboot #10 (20 Nov 12:59). The workstation's `linux/out/.config` is byte-identical to `/proc/config.gz` (md5 398c20ed...).
- **Kernel config:** `CONFIG_TEE` is not set and there is no MobiCore driver. There is no `/dev/mobicore` and no `/dev/esfp0`. The running DT has no fingerprint, tee or SPI1 node.
- **Vendor partition:** sda19 is mounted read-only at `/mnt/vendor` and `/run/android/vendor`. Waydroid's `vendor.img` has no Trustonic or fingerprint blobs.
- **Android chroot:** `/run/android` has `system` (hwservicemanager, linker64), `dev`, `data/vendor` and `mnt/vendor/efs` (read-write).
- **EFS:** `/run/android/mnt/vendor/efs/TEE/00000000.authtokcont` (2018, device-unique) is present.
- **Watchdog:** `/dev/watchdog0` is "S3C2410 Watchdog", **active, 30 s**, fed by systemd (`/etc/systemd/system.conf.d/10-zboot-watchdog.conf`, `RuntimeWatchdogSec=30s`). A hard lock that stops userspace resets the phone within about 30 s. This is already armed, so the test plan only verifies it.

## Modules

```
fingerprint/build.sh              # on the workstation: ~/zrn-fp/fingerprint/build.sh
fingerprint/driver/mobicore/      # zrn-mobicore.ko  (Samsung GPL gud-exynos9810, ported)
fingerprint/driver/etspi/         # zrn-etspi.ko     (compact rewrite; stock/ = original)
fingerprint/dts/                  # SPI1/ET510/tee nodes, mainline syntax, NOT applied
```

Build them against the workstation's `~/ztos-kernel-waydroid/linux` with
`O=out` (the running kernel's tree) and the Arm GNU 14.2 toolchain. Built
2026-11-21:

```
b423c08a3b46c6f5ae4f8bcddaff539a7a045d02afbbfed5d4b80b2b641c0cbf  zrn-mobicore.ko
9b1de4c8f76ccf6e0e55e82e52d3b441e01f15d5b446cc0b5a79474fac0b9849  zrn-etspi.ko
vermagic=7.1.0-zboot SMP preempt mod_unload aarch64
```

Copies are in `out/fingerprint/`. The tree has no `Module.symvers`, so every
undefined symbol of both modules was checked against the phone's
`/proc/kallsyms` `__ksymtab_*` list. All of them resolve.
`cpu_coregroup_mask` (not exported) was replaced.

### zrn-mobicore: changes from stock

- **7.1 API:**
  - `access_ok`;
  - `timespec64`;
  - `get_user_pages_remote`, with no `FOLL_CMA`;
  - `mmap_lock`;
  - `release_pages`;
  - kthread worker init;
  - `pm_freezing`;
  - `vm_flags_set`;
  - `MAX_PAGE_ORDER`;
  - `class_create`;
  - shash flags.
- **No DT node:** `MC_DEVICE_PROPNAME` is off. The notify IRQ is mapped from GIC SPI `irq_spi=233` (edge rising, as `request_irq` asks).
- **`MC_DELAYED_TEE_START`:** **insmod makes no SMC.** mcDriverDaemon's first open of `/dev/mobicore` starts the TEE, or a write of `1` to `/sys/module/zrn_mobicore/parameters/start` does.
- **No SWAP_CPU at probe:** the stock probe's `mc_switch_core(1)` SWAP_CPU SMC is removed, because it would precede MC_FC_INIT. Kinibi stays on CPU0, which ztOS never offlines.
- **Unchanged:** `TBASE_CORE_SWITCHER`, the cpuhp `nq_cpu_down_prep` callback and the stock PM behaviour. On 9810, `MC_PM_RUNTIME` is off, so there is no suspend notifier, exactly as stock.

### zrn-etspi: secure-mode subset of et5xx-spi

- **Device nodes:** `/dev/esfp0` (major 152, class `egis_fingerprint`) and `/sys/class/fingerprint/fingerprint/{type_check,vendor,name,adm,bfs_values,intcnt,resetcnt}`.
- **ioctl `EGIS_IOC_MESSAGE(N)`:**
  - RESET;
  - POWER_CONTROL and POWER_CONTROL_ET5XX;
  - SET_SPI_CLOCK and DISABLE_SPI_CLOCK;
  - INT_TRIGGER_INIT, CLOSE and ABORT, with poll on DRDY;
  - SET_SENSOR_TYPE, ORIENT and SPI_VALUE;
  - CPU_SPEEDUP and LOCKSCREEN (no-ops).

  The non-secure register and frame opcodes return `-EFAULT`, as in the stock secure build.
- **Power order (stock):**
  - on: SPI pins to function 2, then LDO high, wait 1.1 ms, then P0.6 high, wait 10 ms;
  - off: `CS_SET` SiP call, then P0.6 low, then LDO low, then pins inactive.
- **Suspend:** sends the stock `PM_SUSPEND` or `PM_SUSPEND_CS_HIGH` SiP call, but only once the sensor has been powered. Setting `smc_cs=0` turns all SiP calls off.
- **SPI1 clocks:** not switched (`clk_mode=0`; see U3).

## Handshake review: what Kinibi sees first

The trigger is the daemon's open of `/dev/mobicore`, or the `start` parameter.
`mobicore_start()` then runs, in this order:

1. **`MC_FC_MEM_TRACE` (0xFF00000A)** with the phys address of a 1-page log buffer. This is the first SMC. Kinibi returns at once with OK or an error.
2. **`MC_FC_INIT` (0xFF000001)** with the MCI buffer phys base, the NQ offset and length, and the MCP offset and length. The returned flags carry LPAE.
3. **`MC_SMC_N_SIQ` (4).** The first world switch: Kinibi runs until it yields.
4. **MCP `GET_VERSION`** through the NQ. The NWd waits for the notify IRQ, with a 10 s timeout and retries (`mcp_timeout`). The driver accepts MCI major 1 or lower and minor 0.6 or higher.
5. **The scheduler thread** (affinity CPUs 0-3) then YIELDs and N-SIQs whenever the SWd has work.

All fastcalls run on the `tee_fastcall` kthread, which is pinned to CPU0.

What could hang or crash:

- **H1: Kinibi is not resident or not initialised by our boot chain.** The SMC returns an unknown-function error (harmless), or EL3 traps. sboot loads the TEE before any kernel, so it should be resident; this is not proven. A failing step 1 or 2 leaves the module loaded with no TEE: recoverable.
- **H2: MCI buffer in memory Kinibi refuses.** The buffer comes from `__get_free_pages`, normal DRAM. If Kinibi faults while mapping it, the CPU stays in the secure world. CPU0 then hard-locks; the watchdog fires about 30 s later, and only if the other cores also stop feeding systemd.
- **H3: Notify IRQ wrong (SPI 233 not mapped or not routed to the NWd).** Step 4 times out (10 s times the retries). `mobicore_start` then fails cleanly with "No answer after ...". Recoverable.
- **H4: Version or format mismatch.** The driver logs `MCI too old/recent` and stops. Recoverable.
- **H5: SWd long-runs inside N_SIQ or YIELD.** CPU0 is busy in the secure world; the other cores keep running and feeding the watchdog. A Kinibi crash usually reports through the `mc_fc_info` status as HALT, and the driver dumps it (`DUMP_TBASE_HALT_STATUS`).
- **H6: Sleep (zrn-sleepd SYS_SLEEP) with Kinibi initialised.** Stock's 9810 config has no suspend notifier either, so this is as stock. Sleep has not been tested with the TEE up, though. Hold sleep during the first trials.
- **H7: Unload.** Per the stuck-module memory, never rmmod. The module exit tears down without telling the SWd. **Reboot to undo.**

## Test plan (owner present; each step needs approval)

Before every step:

- the owner is at the phone, which is on the charger, over USB SSH;
- the screen agent is idle;
- `cat /sys/class/watchdog/watchdog0/state` shows `active` and `timeout` shows `30`;
- sleep is held through `zrn-testmode`, not by stopping zrn-sleepd by hand (see the `test-toggles` skill);
- `dmesg -w` is captured on the laptop.

No step writes EFS, persist, RPMB or any partition.

| Step | Action | Pass | Failure mode | Recovery |
|---|---|---|---|---|
| **P1** | Skipped: the source shows the normal world never touches SPI1 registers, so a devmem read only risks an SError (abort) for no gain | - | - | - |
| **P2a** | `insmod /root/zrn-mobicore.ko`. This makes no SMC | `/dev/mobicore`, `/dev/mobicore-user` and `/sys/kernel/debug/trustonic_tee/` appear, and dmesg shows "MobiCore mcDrvModuleApi" | probe error (IRQ mapping, class) | leave it loaded, reboot later; never rmmod |
| **P2b** | `echo 1 > /sys/module/zrn_mobicore/parameters/start` | dmesg shows product_id `t-base-EXYNOS64-Android-400C...` and `version_mci` 1.x | H1/H3/H4: an error line, recoverable. H2: hard lock | H2: the watchdog resets in about 30 s; if not, the owner holds Power+VolDown 7 s |
| **P2c** | Read `/sys/kernel/debug/trustonic_tee/{structs_counters,last_smc_log...}` and `/proc/interrupts` (SPI 233 count above 0) | IRQ count rises | no IRQs | same as H3 |
| **P3** | Bind-mount `efs` **read-only** into a private mount namespace of the chroot. Copy `TEE/00000000.authtokcont` to the laptop first. Run `chroot /run/android /vendor/bin/mcDriverDaemon -r ...` with the mobicore.rc arguments | the daemon stays up and opens `/dev/mobicore`; trustlets are registered | the daemon wants to write `/efs/TEE` (fails read-only, logs it) | stop the daemon; nothing was written |
| **P4** | `insmod zrn-etspi.ko`. This only sets LDO low and P0.6 low | `/dev/esfp0` and `/sys/class/fingerprint/fingerprint/name` = ET510 | expander bus busy, or zrn-bt not loaded | reboot |
| **P5** | Start `hwservicemanager`, `vendor.trustonic.tee@1.0-service` and `teeregistry` in the chroot, then `vendor.samsung.hardware.biometrics.fingerprint@3.0-service`. Point `/data/vendor/biometrics` at an **empty tmpfs** | `lshal` lists ISehBiometricsFingerprint; the HAL opens the TA (dmesg shows esfp0 power 1, SPI clock request) | the TA refuses (property or attestation checks: U1, U2), the sensor is not detected (U3 clocks), or the HAL waits on Samsung properties | stop the HAL, then etspi power 0 by closing the device |
| **P6** | **Not before separate approval:** a single test enrolment to the tmpfs, then delete it. Only after P5 passes | - | - | - |

## Risks

- **R1 (high): hard lock in the secure world at P2b.** The watchdog should recover it. The owner must be present.
- **R2:** the trustlets check Samsung state (Knox warranty bit, the boot state from sboot, `ro.boot.*` properties). Enrolment may be refused even with everything loaded.
- **R3:** the HAL needs hwbinder and hwservicemanager in the chroot. See memory s9-hub-hwbinder-hang: Waydroid's `/dev/hwbinder` in `/run/android` blocked lhd before. Use a dedicated binderfs instance.
- **R4:** the EFS auth token is device-unique and cannot be recreated. Keep it read-only at all times and back it up first.
- **R5:** zrn-etspi writes the gpp0 bank registers directly (`ioremap`), next to mainline pinctrl. zrn-power's GPIO save and restore covers PERIC0, so these writes survive sleep. Only touch gpp0-0..3 (masked 16 bits).
- **R6:** PCAL6524 read-modify-write race with zrn-bt (BT_EN on P0.7). zrn-etspi holds the adapter lock across its own RMW, but zrn-bt's RMW is unlocked. Fix later: export `zrn_bt_exp_update()` from zrn-bt with a mutex.
- **R7:** the module loads at all, but the kernel log may fill with Kinibi trace output. `mc_logging` copies the SWd log to dmesg.

## Unknowns

- **U1:** does the TA or bauth require a Samsung property environment (`ro.boot.warranty_bit`, `ro.boot.em.*`, `ro.security.*`)? Next: run `strings` over the HAL and libbauthserver for `__system_property_get` users.
- **U2:** does Kinibi accept our NWd driver unchanged? The driver is identical to stock apart from the API ports, so it probably does.
- **U3:** the SPI1 clock gates (CMU_PERIC0 SPI1 PCLK and IPCLK). The mainline clock provider does not expose them, and their state after boot is not known. If the sensor is not detected at P5, add `clk_mode=1` with the gate register offsets taken from Samsung's `cmucal` (not pinned yet; a read-only devmem of the CMU would need approval).
- **U4:** the sensor's DRDY IRQ flags come from the HAL (`pad[0]`). These are Linux IRQF values passed through as-is.

## zrn-fingerprintd (spec only)

**Purpose:** ztOS's fingerprint service. It is a HIDL client of `fps_hal` and
exposes an fprintd-like D-Bus API to the shell (lock screen) and to apps.

**Process:**

- a systemd service `zrn-fingerprintd.service`, After= the TEE and HAL services;
- the Android side runs in `chroot /run/android` with its own binderfs;
- the daemon itself is a glibc program that talks HIDL through a small bionic helper `zrn-fp-hidl`, which runs in the chroot and speaks a line protocol over a UNIX socket. The helper is linked against `android.hardware.biometrics.fingerprint@2.1` and the `@3.0` stubs. Only the helper links bionic.

**HIDL use (IBiometricsFingerprint 2.1):**

- `setNotify(cb)`;
- `setActiveGroup(gid=0, storePath)`, where `storePath` is the vault mount (below);
- `preEnroll()`, `enroll(hat, gid, timeoutSec)` and `postEnroll()`;
- `authenticate(operationId, gid)` and `cancel()`;
- `enumerate()` and `remove(gid, fid)`;
- `getAuthenticatorId()`.

`hat` is a HardwareAuthToken from Gatekeeper. Until a ztOS Gatekeeper exists,
enrolment is gated in software by zrn-passcoded: the passcode must have been
verified within the last 60 s, and the daemon forwards the token that
zrn-passcoded issues.

**D-Bus (system bus), `net.ztos.Fingerprint1` at `/net/ztos/Fingerprint1`:**

- `ListEnrolledFingers() -> as`;
- `EnrollStart(s finger_name)` and `EnrollStop()`;
- signal `EnrollStatus(s result, b done, u remaining)`;
- `VerifyStart()` and `VerifyStop()`;
- signal `VerifyStatus(s result, b done)`;
- `DeleteEnrolledFinger(s name)` and `DeleteEnrolledFingers()`;
- property `ScanType = "press"` and `NumEnrollStages`.

The result strings are fprintd's (`verify-match`, `verify-no-match`,
`verify-retry-scan`, `enroll-stage-passed`, `enroll-completed`, ...), so
fprintd clients port easily.

**Authorization:**

- polkit actions `net.ztos.fingerprint.verify` (active session) and `.enroll` (passcode required);
- the lock screen is the only Verify client while the device is locked.

**Templates:**

- The TA's sealed template blobs (`/data/vendor/biometrics/{fp/User_0,bk,meta,ta,type}`) live in the **zrn-passcoded LUKS2 vault** at `<vault>/biometrics`. zrn-fingerprintd bind-mounts that path onto the chroot's `/data/vendor/biometrics` only while the vault is unlocked.
- Before first unlock after boot, fingerprint is unavailable (the passcode is required), as on Android.
- A wipe deletes the vault directory; there is no other copy.

**Power and sleep:**

- the HAL powers the sensor itself through esfp0;
- the daemon holds a zrn-sleepd inhibit only during an active enrol or verify;
- wake-on-finger (DRDY as a wake IRQ) is a later phase, after H6 is tested.

**Failure handling:**

- if the HAL dies (hidl death recipient), restart it with backoff and report `verify-unknown-error`;
- after three HAL failures in 10 min, disable fingerprint and fall back to the passcode.
