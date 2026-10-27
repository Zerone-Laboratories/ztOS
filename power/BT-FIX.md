# Bluetooth recovery — 2026-10-24

The controller is restored and powered after a normal reboot and a screen-off
SYS_SLEEP cycle. Discovery starts successfully, but no nearby devices were
reported during these tests. **The requirement that scanning find a device is
still unverified.** A nearby device in pairing/discoverable mode is needed to
finish that acceptance check.

## Cause and initial state

The installed root filesystem lacked `bluez`, `bluez-utils`, all Bluetooth
kernel modules, the board-specific HCD firmware, and the cached EFS address.
`zrn-bluetooth.service` was disabled; its address service was inactive;
`bluetooth.service` did not exist. There were no Bluetooth service journal
entries or HCI initialization messages in this boot. `bluetoothctl` and
`hciconfig` were absent. This was an incomplete installed Bluetooth stack.

The installer copied Wi-Fi firmware and other board modules, but omitted the
Bluetooth firmware, packages, modules, and service enablement. That explains
the observed state. No evidence identifies which rootfs installation removed
the earlier working setup. Forced resets, Waydroid, CPU spin, and cpuidle
changes are not established causes of this absence.

A second problem appeared while restoring the old October 18 build artifacts:
those modules were compiled without `CONFIG_COMPAT`; the current October 22
kernel enables it. `struct proto` in `include/net/sock.h` contains a conditional
`compat_ioctl` member. Despite identical `7.1.0-zboot` vermagic, the old
`bluetooth.ko` caused a NULL dereference in `kmem_cache_alloc_noprof`, reached
through `sk_prot_alloc`, `bt_sock_alloc`, and `hci_sock_create` when `btmgmt`
opened its socket. Firmware setup itself succeeded. Those stale modules were
replaced with rebuilt modules, and the phone rebooted to clear that state.
No equivalent socket crash appeared after reboot.

## Durable changes

- Added `bluez` and `bluez-utils` to `os/packages/starlte`; installed both on
  the phone (`5.87-2`).
- Added current-configuration modules to `prebuilts/modules`: `bluetooth`,
  `btbcm`, `hci_uart`, `rfcomm`, `ecdh_generic`, `ecc`, `kpp`, `cmac`, `aes`,
  and `zrn-bt`. The installer's existing module loop now includes them.
- Installer now installs `third_party/s9plus/firmware/bcm4361B2_semco.hcd` as
  `/usr/lib/firmware/brcm/BCM.samsung,starlte.hcd` and enables
  `zrn-bluetooth.service` and `bluetooth.service`.
- Added a BlueZ service drop-in requiring and ordering after
  `zrn-bluetooth-addr.service`. This preserves the existing rule against
  management requests during firmware download and sets the EFS address
  before BlueZ starts.
- Added `/etc/bluetooth/main.conf` with `AutoEnable = true`. BlueZ's hostname
  integration currently presents the controller as `starlte`.
- Applied the same modules, firmware, configuration, service ordering, and
  enablement to the phone. The existing address helper cached
  `50:77:05:8E:75:B9` from EFS.

The board driver and sleep implementation were not changed. HOST_WAKE remains
an enabled wake source; `sleep_hook=Y`, `sleep_power_off=N` retain chip power
and firmware across SYS_SLEEP.

## Module build provenance

Built with the repo's Arm GNU 14.2 toolchain and `build/linux-7.1` source.
Started `out-bt/.config` from the current `out/.config`, enabling modular BT,
BCM HCI UART (H4 and serdev), and RFCOMM. Compared against the phone's
`/proc/config.gz`: differences in the baseline were build-tool detection and
absolute initramfs/firmware paths, not runtime feature settings.

Ran `olddefconfig modules_prepare`, seeded `out-bt/Module.symvers` from
`out/Module.symvers`, then built external-module groups with `M=crypto`,
`M=net/bluetooth`, `M=drivers/bluetooth`, and the absolute `bluetooth/driver`
path. The Bluetooth build used `crypto/Module.symvers`; the UART build used
`net/bluetooth/Module.symvers` through `KBUILD_EXTRA_SYMBOLS`. The existing
`bluetooth/patch-btbcm.py` quirk and 3 Mbaud change were already present.

The broad crypto group used `KBUILD_MODPOST_WARN=1` for unrelated crypto
modules whose library modules were not built in that invocation. Only the
five required crypto modules listed above were shipped. Bluetooth and UART
modpost completed without unresolved symbols. `depmod` resolved the complete
HCI dependency chain on the phone, and runtime load/setup/socket use passed.
Future kernels/configurations require rebuilding these modules; matching
vermagic alone does not prove ABI compatibility.

## Verification

- Normal reboot: boot ID `d81715c4-de60-4c8f-85f4-0cc61a59c395`.
  `bluetooth` and `zrn-bluetooth` became active automatically; address setup
  completed before BlueZ. `bluetoothctl list/show` and `btmgmt info` showed
  `50:77:05:8E:75:B9`, `Powered: yes`, BR/EDR and LE enabled.
- Firmware: BCM4361B2 Crown E32A ANT1, version `0110.0232`, build `0232`.
  The transmit-power quirk message is expected, not a setup failure.
- Screen-off sleep: ran the installed `zrn-sleepd once 8` under a detached
  systemd unit, stopping/restarting the daemon around this explicit cycle.
  Kernel logged `PM: suspend entry (deep)` and exit. Backend `entries` became
  `1`; helper reported `once: woke: rtc wake_stat=0x80000002`.
  Bluetooth stayed powered, and discovery started again after resume.
  Bluetooth, attachment, and sleep daemon were active afterward.
- Repeated BlueZ scans and a 45-second `btmgmt find` started discovery, but
  returned no devices. No HCI timeout or socket Oops was found after reboot.
  This proves discovery can start, not that RF reception finds a device.
- `hciconfig` is not provided by the installed BlueZ tools; `btmgmt info`
  supplied controller status instead. `rfkill` cannot open `/dev/rfkill`
  because this kernel has `CONFIG_RFKILL` unset; that is not a soft block.
- `btmon` cannot capture on this kernel: `Failed to enable credentials:
  Operation not supported`. No packet-level reception claim is made.
- Installer Python parsing, embedded shell `bash -n`, and `git diff --check`
  passed. A full image build was not run.

Evidence: [reboot scan](bt-fix/reboot-scan.log),
[sleep cycle](bt-fix/sleep-check.log), [long discovery](bt-fix/long-scan.log),
[final scan](bt-fix/final-scan.log), [final state](bt-fix/final-state.log).
The sleep log includes a read of a nonexistent optional `last_sleep_us`
parameter; the checked-in helper omits that read. The authoritative sleep
proof is the entry counter, deep suspend messages, and RTC wake result.
Discovery-stop errors came from separate CLI sessions after the original
requesting client exited; controller status showed discovery off afterward.

All phone access used `python /tmp/ztos-phone-control.py run|sudo|put|get`.
No login failures occurred. Existing unrelated repository edits were retained.
