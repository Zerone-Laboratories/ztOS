# ztOSIAL — design

Written 2026-10-26; implemented for BETA-1 (0.2.0) the same night.

## Status (BETA-1)

| Step | State |
|---|---|
| 1. Spec | done: `ial/spec/`, 13 interfaces; `ztos-ial check` |
| 2. Read-only interfaces | done: `ztos-iald` with generic backends (`ial/ztos_ial/backends/generic.py`), the S9's (`devices/samsung-starlte/vendor/ztos/ial/starlte.py`); `libztos-ial` (C, LGPL) and the Python client (`ztos.ial` in the SDK); ztMETRO takes its backlight and battery from Display1 and Power1, Settings and the hardware test their nodes through `core/ztos-hw.h` |
| 3. Policy interfaces | done for Perf1: zrnperfd takes its caps, thermal sensors, GPU and battery nodes from `/run/ztos-ial/Perf1.env` (`perfd/perfial.c`), the S9's as fallback. Sleep1 serves zrn-sleepd's state; zrn-sleepd itself is unchanged |
| 4. Optional interfaces | served (Sensors1, Haptics1, Lights1, Modem1, CallAudio1, AudioDevice1, Accel1, Android1); the apps still talk to the S9 daemons directly for calls, SMS and sensors |
| 5. Packages and `/vendor` | layout done, not pacman packages yet: the installer builds the GSI (`ztos build --target gsi`, a tarball) and adds a device layer (`devices/<dir>/layer.sh`, `vendor/`, `overlay/`, `packages.txt`); the running S9 was moved in place (`devices/samsung-starlte/migrate-beta1.sh`) |
| 6. Waydroid in the GSI | done: `ztos-waydroid-images` fetches, checks and strips the pinned image at first boot; Waydroid is in `os/packages/common` |
| 7. Split repositories | ready, not run: `scripts/split/split.py` (`preview` / `git`), rules in `scripts/split/map.txt`; needs ztOSMobile committed first |
| 8. ztConflo | done: `--gsi`, kernel contract, generated device layers, `ztos-ial check`, `ztos-ial test` in promote; the S9 is the registry's first native entry |
| 9. BETA-1 | VERSION 0.2.0-BETA-1 |

The user is no longer hard-coded: `/etc/ztos/user.conf` (written at install
and first boot; UID 1000's passwd entry without it), read through
`/usr/lib/ztos/user.sh`, `ztos_user.py` and `ztmetro/ztos-user.h`;
`ztos-user-dropins` writes what cannot read it.

Decisions taken (2026-10-26): the Waydroid image ships as a recipe only;
`libztos-ial` and the interface specs are LGPL-2.1-or-later; the system stays
read-write (version 2's read-only `/usr` is not planned for now); where the
pacman repository lives is still open.

## Summary

ztOS splits into a **generic system** that is the same on every phone and a
**device layer** that is different on each one. The two meet only at
**ztOSIAL**, the Zerone Technologies Operating System Interface Abstraction Layer: a set of small, versioned
interfaces for the hardware that standard Linux does not already cover.

- The generic system is built once per release for all aarch64 phones: the
  **ztOS GSI** (generic system image). It holds ztMETRO, the apps, the SDK,
  the generic services, the Android runtimes (ATL and the stripped Waydroid)
  and generic ztOSIAL backends.
- A device layer holds the kernel, modules, firmware, boot setup, a manifest
  and the device's own ztOSIAL backends.
- Porting ztOS to a phone means writing a device layer. Nothing in the GSI
  changes for a new phone.
- ztConflo builds both halves and puts them together, for phones in the ztOS
  registry and, through postmarketOS, for phones nobody has ported yet
  (cross-porting).

This is ztOS's equivalent of Android's Project Treble: GSI against the
vendor image, HIDL/AIDL against ztOSIAL, VINTF against the device manifest.

## Why now

The Galaxy S9 port is the only real device, and its knowledge is spread
through the whole tree. Examples from today's code:

| Where | What is S9-only |
|---|---|
| `ztmetro/perfd/zrnperfd.c` | cluster layout, `BIG_KHZ_1/2/4` caps, `s9p_cpufreq` temperatures, `17500000.gpu` devfreq |
| `rootfs/overlay/usr/local/sbin/ztos-panel-bl`, ztMETRO | Samsung AOR brightness table, `/sys/class/backlight/s9p-panel` |
| ztMETRO, Power Management, perfd | battery at `max170xx_battery`, charger at `max77705-charger` |
| `zrn-sleepd` | Exynos SoC sleep, its wake sources and traps |
| `ztoscsf-sensord`, `ztoscsf-sensorhub` | Samsung SSP sensor hub (`/sys/class/sec/sensorhub`) |
| `ztoscorenetworkingd`, `zrn-modem` | Samsung Shannon modem (`/dev/umts_ipc0`) |
| `snd-zrn-abox`, `ztoscirrusaudio`, `ztos-speaker-cal` | ABOX DSP, MAX98512 amps, call audio routes |
| `zrn_haptic`, `zrn_led`, `zrn-hotplug`, `zrn-cpuidle`, `zrn-score` | S9 drivers that apps check by module name |

Every one of these blocks a second phone. ztConflo can already build an
image for 135 mainline phones, but ztMETRO on them finds none of the paths
above.

## Goals and non-goals

Goals:

- One GSI per release, booted unchanged on every supported phone.
- A device layer that a community porter can write without reading ztMETRO's
  source.
- Each interface versioned, with a compatibility check at build time and at
  boot.
- Generic backends good enough that a phone with full mainline support (a
  postmarketOS `main` device) needs no device code beyond its registry entry.
- The stripped Waydroid in every GSI, on by default where the kernel allows it.
- ztConflo builds GSI + device layer for registry phones and for
  postmarketOS phones alike.
- The S9 keeps working at every step of the move.

Non-goals:

- Downstream (vendor Android) kernels. Unchanged from ztConflo: no Mesa GPU
  driver, no ztMETRO.
- 32-bit phones.
- A stable in-kernel ABI. Kernel and modules always ship together in the
  device layer.
- Wrapping what Linux already standardises (see the next section).

## What is not in ztOSIAL

Standard Linux interfaces are used directly by the GSI, on every phone:

| Area | Standard used | Device layer's job |
|---|---|---|
| Display scanout | DRM/KMS, Wayland (wlroots) | a working DRM driver |
| GPU | Mesa (freedreno, panfrost, panthor…) | the kernel driver, firmware |
| Touch, buttons | evdev / libinput | drivers; quirks in `/vendor/ztos/udev` |
| Audio streams | ALSA, PipeWire, ALSA UCM | driver, UCM profile |
| Wi-Fi | NetworkManager, cfg80211 | driver, firmware |
| Bluetooth | BlueZ | driver, firmware, address setup at boot |
| Camera | libcamera | driver, IPA tuning (when it exists) |
| Storage, USB gadget | block devices, configfs | partitions, gadget UDC name in the manifest |

Device bring-up (loading firmware, setting the Bluetooth address, starting a
Qualcomm `rmtfs`) is a systemd unit in the device layer, not an interface.

## The interfaces

Each interface is a D-Bus interface on the system bus, named
`org.ztos.ial.<Area><Major>`, with its spec as introspection XML in
`ztos-ial/spec/`. Why D-Bus:

- Language-neutral: porters write backends in C, Python or Rust.
- Process isolation, as in Treble: a crashing backend restarts under systemd
  and does not take ztMETRO with it.
- Introspection, polkit for privileged calls, bus activation for backends
  that are only needed sometimes.
- ztOS already depends on it (BlueZ, NetworkManager, logind).

Hot paths stay off the bus. A high-rate stream (sensor samples, modem audio)
is a file descriptor passed once over D-Bus, then read directly. Policy loops
(zrnperfd, every 250 ms) read sysfs paths the backend names, rather than make
a D-Bus call per sample.

Version rules: a major version (`Power1` → `Power2`) breaks compatibility and
is a new interface; both can be served during a transition. Minor additions
(new methods, properties, signals) raise the `Version` property and never
remove anything.

### Version 1 set

| Interface | Required | Covers | Generic backend | S9 backend (today's code) |
|---|---|---|---|---|
| `Device1` | yes | model, codename, manifest, capabilities, form factor, scale | from manifest | from manifest |
| `Power1` | yes | battery (capacity, current, voltage, temperature, status), charger (online, type, input limit), charge control | `power_supply` class | max1720x + max77705, `zrn-charge`, `zboot-charge-hold` |
| `Perf1` | yes | CPU topology and frequency tables, per-cluster caps by cores online, cores that must stay online, idle-state constraints, thermal sensors and trip points, GPU devfreq node | cpufreq, thermal, devfreq; conservative caps | `s9p_cpufreq`, `zrn-hotplug`, `zrn-cpuidle` (mask 0x0c), big-core caps |
| `Display1` | yes | brightness on a 0..1 perceptual scale to the panel's units, panel power, ambient-light curve | `backlight` class, linear | `ztos-panel-bl`, AOR table, `ztos-panel-selfmask` |
| `Sleep1` | yes | suspend method (s2idle, deep, SoC sleep), wake sources, inhibitors, "safe to sleep" checks, resume hooks | `systemd-sleep`, s2idle | `zrn-sleepd` (SYS_SLEEP, BT wake, Wi-Fi quirk) |
| `Sensors1` | no | accelerometer, gyroscope, light, proximity, magnetometer, steps; claim/release; fd stream | iio / `iio-sensor-proxy` | `ztoscsf-sensord`, SSP hub |
| `Haptics1` | no | named patterns (tap, click, notify), amplitude, duration | evdev force feedback | `zrn_haptic` |
| `Lights1` | no | notification LED colour and blink, torch | `leds` class | `zrn_led` |
| `Modem1` | no | radio power, SIM, network, data, SMS, calls, signal | ModemManager | `ztoscorenetworkingd`, `zrn-modem` |
| `CallAudio1` | no | voice-call route (earpiece, speaker, headset, Bluetooth), mute, in-call volume | ALSA UCM `Voice Call` verb | ABOX voice path, CP VSS, `ztoscirrusaudio` |
| `AudioDevice1` | no | speaker protection and calibration, amp health, jack and button state | none (nothing to do) | `ztos-speaker-cal`, DSM, `snd-zrn-abox` parameters |
| `Accel1` | no | an inference accelerator for small models (IntelligentBoost) | CPU | `zrn-score` (SCORE DSP) |
| `Android1` | no | what the Android container needs from the device: GPU mode for Waydroid (`gbm`, `minigbm`), codec hints, sensor and haptic bridges, memory budget | from kernel config and manifest | manifest values used today in `waydroid.cfg` |

A capability the device lacks is absent from the manifest. ztMETRO and the
apps ask `Device1.Capabilities` and hide what is not there (the dialer
without `Modem1`, the auto-brightness toggle without a light sensor), instead
of probing module names as they do now.

### Client side

`libztos-ial` (C, with Python bindings for the SDK) wraps the interfaces:
connect, version check, typed calls, change signals. ztMETRO, the apps and the
generic services use only this library, never a device path. ztOS SDK apps
get the same through `ztos.ial`.

## The device manifest

Every device layer ships `/vendor/ztos/manifest.toml`:

```toml
[device]
codename = "samsung-starlte"
name = "Samsung Galaxy S9"
form_factor = "phone"
scale = 2.5

[gsi]
min = "0.2.0"                 # oldest GSI this layer works with

[ial.Power1]
backend = "vendor"            # vendor | generic
service = "ztos-ial-power-starlte.service"
version = 1

[ial.Perf1]
backend = "vendor"
service = "ztos-ial-perf-starlte.service"
version = 1

[ial.Display1]
backend = "generic"           # generic backends need only parameters
node = "/sys/class/backlight/s9p-panel"
curve = "table:/vendor/ztos/brightness.csv"

[ial.Modem1]
backend = "vendor"
service = "ztos-ial-modem-starlte.service"
version = 1

[android]
binder = "binderfs"           # binderfs | none
gralloc = "gbm"
memory_budget_mb = 1600

[boot]
usb_gadget_udc = "10c00000.dwc3"
```

`ztos-ial check` compares the manifest against the GSI's required interfaces
and versions. ztConflo runs it at build time and refuses an image that fails;
the GSI runs it at boot and shows the gaps in Settings > About.

## Image layout

Version 1 keeps one root filesystem, because most phones flash ztOS into one
partition (`userdata`). Ownership is by package, so the two halves can still
update apart:

| Path | Owner | Contents |
|---|---|---|
| `/usr`, `/etc` defaults | GSI packages (`ztos-*`) | ztMETRO, apps, SDK, generic services, generic backends, Waydroid and ATL |
| `/vendor` | device packages (`ztos-vendor-<codename>`) | manifest, backends, units, udev rules, UCM profiles, brightness tables, firmware overrides |
| `/usr/lib/modules/<kver>`, `/boot` | device packages (`ztos-kernel-<codename>`) | kernel, modules, dtb, initramfs |
| `/etc`, `/var`, `/home` | the user | settings and data |

A device unit is installed as `/vendor/ztos/systemd/*.service` and linked in
by a generator, so the GSI's `/usr/lib/systemd` holds no device units.

Version 2 makes the split physical where the phone allows it: the GSI as a
read-only erofs image mounted on `/usr`, the device layer as a second image
on `/vendor`, with A/B slots for updates. Swapping the GSI image is then the
whole OS update, as on Android.

## Waydroid in every GSI

The stripped Waydroid (`android/waydroid/`) becomes part of the GSI.

- Package `ztos-waydroid`: Waydroid, our services (`ztos-waydroid-boot`,
  `-session`, `-network`, `-audio-reconnect`), the launcher, the background
  policy, the storage repair, the app-list sync, the `~/Android` link.
- The stripped system image is built by `strip-image.sh` from the pinned
  Waydroid Android 13 image. The GSI ships a recipe, not the image: a
  first-boot unit downloads the pinned image, checks its SHA-256, strips it
  and installs it in the background. Until it finishes, Android apps show as
  "preparing". (Shipping the stripped image prebuilt is an open question
  below.)
- On by default when the kernel has binderfs (`[android] binder` in the
  manifest, checked against the running kernel). Without binder, Waydroid is
  installed but hidden, and ATL still runs Android apps.
- Device-specific values (gralloc mode, memory budget, which sensors and
  haptics to bridge) come from `Android1` and the manifest, not from a
  hand-edited `waydroid.cfg`.
- Required kernel options are part of the GSI's kernel contract (below);
  ztConflo checks them in the kernel it builds or reuses.

## Kernel contract

The GSI needs these from any device kernel. ztConflo checks them from the
built kernel's config; `ztos-ial check` checks them at boot.

| Need | Options | Without it |
|---|---|---|
| systemd, cgroups v2, namespaces | `CGROUPS`, `MEMCG`, `CPUSETS`, `NAMESPACES`, `PSI` | no boot (hard requirement) |
| GPU | the Mesa driver's kernel side | no ztMETRO (hard, already enforced) |
| zram, PSI memory pressure | `ZRAM`, `PSI` | memory policy off |
| Waydroid | `ANDROID_BINDER_IPC`, `ANDROID_BINDERFS`, `MEMFD_CREATE`, `VETH`, `BRIDGE`, `NF_TABLES`, `NETFILTER_XT_*` | Waydroid hidden |
| USB networking | `USB_CONFIGFS_NCM` or `_ECM`, `_RNDIS` | no SSH over USB |
| Evdev force feedback | `INPUT_FF_MEMLESS` | generic haptics off |

## Repositories

| Repository | Contents | Released as |
|---|---|---|
| `ztos-core` | ztMETRO, apps, SDK, generic services (perf policy, memory policy, store, updater), Android runtimes (ATL glue, `ztos-waydroid`), `os/` package lists, overlays and wallpapers | `ztos-*` pacman packages, GSI |
| `ztos-ial` | interface specs (XML), `libztos-ial` and Python bindings, `ztos-ial check` and `ztos-ial test`, generic backends, porting guide | `ztos-ial*` packages, part of the GSI |
| `ztos-device-samsung-starlte` | kernel patches and config, device tree, out-of-tree modules (abox, bbd, haptics, LED, modem, SCORE, hotplug, cpuidle), firmware, S9 backends, units, manifest, boot image build (uniLoader) | `ztos-kernel-starlte`, `ztos-vendor-starlte` |
| `ztos-build` | GSI assembly, release signing, the pacman repository, the installer (`installer/ztos`), QEMU target | GSI images, installer |
| `ztconflo` | unchanged role: codename in, flashable image out | — |
| `ztos-devices` | the registry; gains `[ial]`, `[android]` and `[source.native]` | — |

The QEMU target becomes a device layer too (`ztos-device-qemu-virt`), so the
emulator tests the same GSI as the phone.

What moves where, from today's `ztOSMobile`:

| Today | Goes to |
|---|---|
| `ztmetro/` (shell, apps), `sdk/`, `apps/`, `os/` (except `overlay-starlte`, `packages/starlte`) | `ztos-core` |
| `android/waydroid/`, `android/atl/`, `os/overlay/usr/local/bin/ztos-android` | `ztos-core` (`ztos-waydroid`, `ztos-atl`) |
| `ztmetro/perfd` | `ztos-core` as the policy engine; topology, caps and sensors read from `Perf1` |
| `ztmetro/sensord`, `ztmetro/modemd`, `ztmetro/csf` | split: client side to `ztos-core`, S9 backends to the device repo |
| `kernel/`, `scripts/build-boot.sh`, `initramfs/`, `firmware/`, `prebuilts/modules` | `ztos-device-samsung-starlte` |
| `audio/`, `sensors/`, `haptics/`, `leds/`, `modem/`, `score/`, `power/`, `bluetooth/`, `camera/` | `ztos-device-samsung-starlte` |
| `rootfs/overlay` | split by file: generic to `ztos-core`, S9 to the device repo |
| `installer/` | `ztos-build` |
| `third_party/` | stays with whichever repo uses it (the S9+ port and the vendor kernel with the device repo) |

History: each repository is cut from `ztOSMobile` with `git filter-repo`
on its paths, so history follows the files. `ztOSMobile` stays as an
archive with a pointer to the new repositories.

## ztConflo

ztConflo keeps its job and its resolution order (registry, then
postmarketOS). What changes is what it assembles.

New pipeline:

1. **GSI.** Take the GSI for the requested release: download the signed GSI
   (or build it once with `ztos-build`), verify it, cache it in
   `~/.cache/ztconflo/gsi/<version>/`. Shared by every phone, so building a
   second phone does not rebuild ztMETRO under qemu.
2. **Kernel side.** As now: `origin = "pmos"` builds the pmaports kernel,
   firmware and initramfs; `origin = "native"` installs `ztos-kernel-<codename>`
   from the ztOS repository, or builds it from `[source.native]`.
3. **Kernel contract.** Read the kernel config; check the table above. Hard
   failures stop the build; Waydroid and other soft gaps are written into the
   manifest and reported.
4. **Device layer.** Native phones: install `ztos-vendor-<codename>`.
   postmarketOS phones: generate a device layer from the registry entry:
   a manifest with generic backends, `[userspace].daemons` as vendor units,
   the entry's `overlay/` under `/vendor`, and UCM and udev files from the
   pmaports device package.
5. **Assemble.** Unpack the GSI, add the device layer, modules and firmware,
   configure (user, hostname, `[ztmetro]`), and run `ztos-ial check` inside the
   root under qemu.
6. **Images.** `boot.img` and `rootfs.img` as today; later, `system.img` and
   `vendor.img` for phones with room for version 2.

Registry schema additions:

```toml
[source.native]               # origin = "native"
repo = "https://github.com/Zerone-Laboratories/ztos-device-samsung-starlte"
commit = "<sha>"

[ial]                         # overrides for the generated manifest (pmos phones)
Display1 = { node = "/sys/class/backlight/backlight", curve = "gamma:2.2" }
Haptics1 = { backend = "generic", device = "pmic_vib" }

[android]
binder = "binderfs"
gralloc = "gbm"
```

`ztconflo promote` adds `ztos-ial test` to the hardware checklist: each
manifest interface is called and checked (battery reports, brightness moves,
a haptic pulse is felt, the modem registers). A phone reaches `tested` only
when every interface its manifest declares passes.

## Cross-porting

A postmarketOS phone with mainline support becomes a ztOS phone with no
device code at all: GSI + generated device layer + generic backends. When a
porter needs more (a better brightness curve, a vendor modem), the path is:

1. Override parameters in the registry entry's `[ial]` table.
2. When parameters are not enough, write a backend for that one interface in
   a device repository, and switch the entry to `origin = "native"` for that
   layer.
3. Promote; the entry moves `untested` → `boots` → `tested`.

The S9 is the other direction: a fully native device whose backends are
examples for the porting guide. Because both kinds of phone run the same GSI,
an app or ztMETRO fix reaches every phone in the next GSI release.

## Migration plan

Each step leaves the S9 working and is its own release-candidate build.

1. **Spec.** Write the version 1 interface XML and `ztos-ial check`. No
   behaviour change.
2. **Read-only interfaces first.** `Device1`, `Power1`, `Display1`: S9
   backends wrap today's paths; ztMETRO, Power Management and Settings switch
   to `libztos-ial`. The generic backends are tested on the QEMU target.
3. **Policy interfaces.** `Perf1`, `Sleep1`: zrnperfd and zrn-sleepd split
   into generic policy (core) and S9 backend (device repo). IntelligentBoost,
   the thermal and power limits move with the policy.
4. **Optional interfaces.** `Sensors1`, `Haptics1`, `Lights1`, `Modem1`,
   `CallAudio1`, `AudioDevice1`, `Accel1`, `Android1`.
5. **Packages and `/vendor`.** Package the GSI and the S9 device layer;
   install the S9 from them.
6. **Waydroid in the GSI.** `ztos-waydroid` with the first-boot image
   recipe; on by default.
7. **Split repositories** with `git filter-repo`.
8. **ztConflo** builds from the GSI and generated device layers; first
   postmarketOS phone through the new pipeline to `boots`.
9. **BETA-1**: GSI 0.2.0 and the S9 device layer released together.

## Open questions

- Where do the pacman repository and signed GSIs live: GitHub releases or
  self-hosted (already open in ztConflo's spec)?
