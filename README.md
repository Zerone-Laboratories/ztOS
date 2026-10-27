# ztOS

**ALPHA-2** — Galaxy S9 (SM-G960N / starlte) build.

ztOS is a phone operating system: the **ztMETRO** shell (Windows Phone 8.1's
look and motion) on Arch Linux ARM and mainline Linux, with its own apps, an
updater and an SDK for apps in Python and Qt. The ztMETRO Shell and its components are written in C, and accelerated with OpenGL. Currently its ported for a Samsung Galaxy
S9 (starlte), and I am hoping to expand supports for more devices in the future :).

> This ROM is in very much Alpha, The installer is not tested on real hardware, open a issue in this github repo if you have any issues with it

>Your warranty is now void. I am not responsible for bricked devices, dead SD cards, thermonuclear war, or you getting fired because the alarm app failed. Please do some research if you have any concerns about features included in this ROM before flashing it! YOU are choosing to make these modifications, and if you point the finger at me for messing up your device, I will laugh at you.

**To install it:** `installer/ztos` (see [installer/README.md](installer/README.md)).
The installer lets you choose between:
1. **Auto Download Prebuilt image and install** (fastest, uses GitHub Release images with default login `zerone` / `toor`)
2. **Build image in this PC and install** (custom user/password, builds via QEMU user emulation)
3. **Build image in this PC and store** (builds and exports the image without flashing)

**Jump to:** [Screenshots](#screenshots) · [Status: what works](#status) · [**Known quirks**](#known-quirks) (read before flashing) · [What is where](#what-is-where) · [The SDK](sdk/README.md) · [License](#license)

## Screenshots

From a Galaxy S9 running ztOS (taken from the live display with `scripts/phone-grab.sh`).

<table>
<tr>
<td align="center"><img src="docs/screenshots/start.png" width="230"><br><b>Start</b></td>
<td align="center"><img src="docs/screenshots/app-list.png" width="230"><br><b>App list</b></td>
<td align="center"><img sr[bluetooth](bluetooth)c="docs/screenshots/networking.png" width="230"><br><b>Cellular + SIM</b></td>
<td align="center"><img src="docs/screenshots/sensors.png" width="230"><br><b>Sensors</b> (ZrnDevTest)</td>
<td align="center"><img src="docs/screenshots/about.png" width="230"><br><b>About</b></td>
</tr>
</table>

## What is where

| | |
|---|---|
| `installer/` | `ztos`, the installer: builds ztOS and installs it in QEMU or on the phone |
| `os/` | what makes the system ztOS: its packages, files (identity, session, updater, default tiles, fonts) and apps (Welcome, Updates) |
| `ztmetro/` | the shell, its toolkit (ZrnMetroCore), the apps written with it (Settings, ZrnDevTest, Phone, Messaging, People) and the system daemons |
| `sdk/` | the ztOS SDK: apps in Python and Qt ([sdk/README.md](sdk/README.md)) |
| `kernel/`, `initramfs/`, `scripts/` | the phone's boot image: mainline 7.1 with the Exynos 9810 drivers, the start-up program, flashing |
| `audio/`, `modem/`, `sensors/`, `power/`, `leds/`, `haptics/`, `firmware/` | the phone's hardware support, as loadable kernel modules |
| `rootfs/` | the phone's own system files (USB link, charger, fallbacks) |
| `stock/` | the phone's stock firmware and its EFS backups: never modified |
| `archive/` | everything else from bring-up (recordings, screenshots, logs); `archive/MANIFEST.txt` says where each came from |
| `build/`, `out/` | build inputs and outputs (not in the repository) |

## ztOS's parts

| Name | What it is |
|---|---|
| ztMETRO | the shell: Start, the app list, the action centre, the keyboard, the lock screen |
| ztOSCoreSensorFramework (`ztoscsf`) | starts and watches every piece of hardware support, and reports what fails; runs the sensor hub (`ztoscsf-sensorhub`) and the sensors (`ztoscsf-sensord`) |
| ztOSCoreNetworking | the modem (`ztoscorenetworking-modem`), calls, messages and mobile data (`ztoscorenetworkingd`), and Wi-Fi joining (`ztoscorenetworking-wifi`) |
| ztOSCirrusAudio (`ztoscirrusaudio`) | sound: the audio DSP and the Cirrus speaker amplifiers |
| zrnperfd | power: dynamic boost and core power-down |
| ztos-update | the updater, with the Updates app |

---

## Status

Samsung Galaxy S9 (SM-G960N, `starlte`, Exynos 9810), mainline Linux 7.1. In
the style of postmarketOS's device pages: what was tried on the phone itself
(*tested*) and what is written but not yet exercised with real use (*untested*).

**Works**: tried, and does what it should. **Partial**: some of it works; the
notes say what. **Broken**: tried, does not work. **Not started**: nothing yet.

### Hardware

| Category | Feature | Status | Notes |
|---|---|---|---|
| Boot | Booting from internal storage (UFS) | Works | Falls back to TWRP by itself after a crash or watchdog reset |
| Boot | Charging while off | Partial | Off with a charger in, it stays in a minimal start until the battery reaches 10 %; written, and not yet tried from below 10 % |
| Display | Panel, 1440x2960 | Works | DECON scanout |
| Display | Brightness | Works | Manual, and automatic from the light sensor |
| Display | Colour tone | Works | OpenTone in ztoscsf-sensord: the hub's light sensor reports the room's colour temperature, and the screen's white follows it part of the way (5000–6500 K); Settings > display |
| Input | Touchscreen | Works | S6SY761, multi-touch |
| Input | Power and volume keys | Works | Screen off and on; volume steps with the on-screen indicator |
| Input | Vibration | Works | Strength per kind of touch, set in Settings > haptics |
| Input | Notification light | Works | Charging, charged, low battery (breathing), rapid boost (white blink) |
| 3D | GPU | Works | Mali-G72 MC18 through Panfrost, OpenGL ES 3.1 |
| CPU | All 8 cores, frequency scaling | Works | The big cores are powered down when idle and brought back on demand (zrnperfd) |
| CPU | Suspend / Idle Power-Down | Works | Per-core C2 power-down active on efficiency cores via zrn-cpuidle with MCT hardware broadcast timer; big cores dynamically offlined via zrnperfd with Rapid Boost |
| Power | Battery and charger | Works | MAX17042 gauge, MAX77705 charger |
| Network | Wi-Fi | Works | BCM4361; join, forget, details and per-network signal in Settings; PCIe link and Wi-Fi reconnect passed two AP hardware-sleep cycles |
| Network | Bluetooth | Not started | |
| Network | Mobile data (4G) | Works | Samsung IPC through ztOSCoreNetworking; access point set in Settings |
| Network | USB networking to a computer | Works | 172.16.42.1 (the computer gets 172.16.42.2), plus a serial console |
| Modem | SIM, registration, signal | Works | The modem runs exactly as stock does; EFS (IMEI, calibration) is never altered |
| Modem | SMS | Partial | Sending works; receiving is written and untested with a real message |
| Modem | Calls | Partial | Placing and answering connect; the call's sound is not routed, so nobody is heard |
| Audio | Speaker | Works | The audio DSP and the Cirrus amplifiers, through ztOSCirrusAudio |
| Audio | Microphone, headphones | Not started | |
| Sensors | Ambient light | Works | Through the sensor hub (ztOSCoreSensorFramework) |
| Sensors | Motion (accelerometer, gyroscope) | Partial | Readable in ZrnDevTest; nothing in the shell uses it yet |
| Camera | Cameras | Not started | |
| Location | GPS | Not started | |
| Other | NFC, fingerprint, Bixby key | Not started | |

### Software

| Category | Feature | Status | Notes |
|---|---|---|---|
| Shell | ztMETRO: Start, tiles, app list, search, lock screen | Works | Windows Phone 8.1's look and motion, measured against reference video |
| Shell | App switcher, action centre | Works | |
| Shell | On-screen keyboard | Partial | Works; the new behaviour (it lies over the app and lifts it so the text box stays in sight, instead of resizing the app) is written and not yet seen working |
| Shell | Notifications, media banner | Works | A freedesktop notification service, plus a push client (ntfy) |
| Shell | Haptics | Works | |
| Power | Dynamic boost | Works | Efficiency cores only until the app in front needs more; then big cores, then everything |
| Apps | Settings | Works | Wi-Fi, mobile, haptics, quick actions, theme and more |
| Apps | Phone, Messaging, People | Works | In the same style as Settings |
| Apps | ZrnDevTest | Works | What the hardware reports |
| Apps | Firefox, Chromium, terminal | Works | Ordinary Linux apps, shown by ztMETRO as windows |
| System | Updates | Works | A daily check, a notification, and an Updates app (tested in the emulator) |
| System | Installer (`installer/ztos`) | Partial | QEMU builds, installs and boots; the phone flow is implemented but not yet validated end to end on a fresh install |
| SDK | ztOS SDK (apps in Python and Qt) | Works | Build, install, run, logs, uninstall from the app list; first start is slower than the native apps' |
| Emulator | ztOS in QEMU | Partial | Boots and runs; slow, because the phone is fully emulated |

## Known quirks

Things that can go wrong on the phone, and what to do. Some are unfinished
engineering, some are the price of running Samsung's own firmware.

**1. The modem or the sensor hub does not come up, or goes offline.** The
signs: no signal and no registration under Settings > cellular + SIM, and no
data, SMS or calls (modem); the light sensor reads nothing, automatic
brightness stops and ZrnDevTest's sensors page is empty (sensor hub). It
tends to follow a forced reboot (Power + Volume Down) or a crash. What brings
them back is **one pass through TWRP**: reboot into recovery, then back into
ztOS.

    systemctl reboot --reboot-argument=recovery     # or hold Volume Up + Bixby + Power
    adb shell reboot                                # once TWRP is up

Samsung's own kernel in TWRP powers the hub's supply and sets up its pins at
boot, which ours does not yet do (the details and the register dump to compare
are in [sensors/README.md](sensors/README.md)). Try a service restart first
(`ztoscsf-sensorhub`, or `ztoscorenetworking-modem`; the networking daemon
follows it). Restarting only the daemon leaves the modem deaf until the modem
service is restarted too. (A phone installed before the component names were
settled has them as `zrn-sensorhub`, `zrn-modem` and `zrn-modemd`;
`scripts/dev/migrate-names.sh` moves it over.)

**2. A driver that crashed while loading is not loaded again at the next
boot.** This stops a crash from becoming a boot loop. The modem, the sensor hub
and sound each leave a mark file while their driver loads
(`/var/lib/ztoscorenetworking/modem-loading`, `/var/lib/ztoscsf-sensorhub/loading`,
`/var/lib/ztoscirrusaudio/loading`); if it is still there at the next boot, the
driver is skipped. Remove the mark and reboot to try again.

**3. Do not `rmmod` a driver module to try a new build: reboot instead.** A
failed unload has left the phone unable to shut down.

**4. Powering down cores needs `zrn-hotplug.ko`.** Without it, a core taken
offline on this kernel does not come back. zrnperfd only touches cores when
the module is loaded, and never takes the last core of a cluster offline.

**5. There is no suspend.** The big cores do not wake from it, so with the
screen off the phone runs at its lowest clocks instead. Idle battery use is
higher than Android's.

**6. The clock resets at every boot** until the phone gets the time from the
network. Offline, the lock screen can show an old date, and anything built on
the phone needs `date -s` first.

**7. ztOS needs the stock Android partitions to stay on the phone.** The
modem's boot daemon (`cbd`) and the sensor hub's daemon (`lhd`) are Samsung's
own Android programs, run from a read-only root assembled from the phone's
SYSTEM and VENDOR partitions (`zrn-android-root`). The installer writes only BOOT
and USERDATA; do not wipe the others. The EFS partitions (IMEI, calibration)
are never touched.

**8. Calls connect, but nobody is heard.** The call's audio is not routed yet.

**9. Automatic brightness and the slider fight.** While automatic brightness is
on, the Settings slider and the action centre tile move the screen only until
the light in the room next changes.

**10. An SDK app's first start is slower than a native app's.** It loads Qt for
Python (about a second more); the warm runtime that removes most of that is
started by the first SDK app after boot.

**11. The emulator is slow, and its screen is black for the first minutes.** The
whole phone is emulated on your computer's CPU. It draws with software OpenGL by
default; `ztos emulator --gl` uses your GPU, but some drivers (NVIDIA, on laptops
with two GPUs) make QEMU stop with `eglMakeCurrent failed: EGL_BAD_ACCESS`.

## The phone port: mainline Arch Linux ARM on a Galaxy S9

Arch Linux ARM on **mainline Linux 7.1**, booting from the phone's internal UFS
on a Samsung Galaxy S9, **SM-G960N** (starlte, Exynos 9810). This is phase 1 of
[the spec](https://claude.ai/code/artifact/d8b8e60d-3e15-4597-a461-0315317ed0a0),
with one change the spec did not anticipate: the kernel is mainline, not the
downstream 4.9, so the spec's top risk — systemd 258 needing Linux ≥ 5.4 — does
not exist. The boot stage is a plain initramfs, not yet the two-entry menu; the
microSD / postmarketOS half is untouched.

## State

| | |
|---|---|
| Kernel | mainline 7.1.0 + the Exynos 9810 drivers of [SheroAbi/galaxy-s9plus-mainline-linux](https://github.com/SheroAbi/galaxy-s9plus-mainline-linux), device tree derived for starlte |
| Userspace | Arch Linux ARM aarch64, systemd, 0 failed units, root grown to 53 G on `USERDATA` |
| Boot | normal power-on → Arch; `reboot recovery` → TWRP; a panic or watchdog reset → TWRP |
| Reached over | USB: `ssh alarm@172.16.42.1` (the host gets 172.16.42.2) and a serial console on `/dev/ttyACM0` |
| Works | all 8 cores with frequency scaling (M3 to 2704 MHz, thermal capped), UFS, USB gadget (NCM + ACM), **GPU: Mesa 26.2 Panfrost on Mali-G72 MC18, GLES 3.1, glmark2 570**, **DECON scanout 1440x2960**, touch (S6SY761), battery + charger (MAX17042 / MAX77705), **Wi-Fi (BCM4361), joined from Settings**, watchdog, **sound: speaker and DSP (ztOSCirrusAudio)**, **mobile network: 4G data, SMS, placing and answering calls (ztOSCoreNetworking)**, sensors (ztOSCoreSensorFramework), notification light, vibration, core power-down |
| Still missing | camera, Bluetooth, the sound of calls (a call connects, but its audio is not routed yet), suspend (the M3 cores do not come back) |

Login: `alarm` / `alarm`, root password `root` (Arch Linux ARM defaults,
unchanged — change them). `sudo` is installed. An SSH key is in place for both
users (`~/.ssh/id_ed25519.pub` of the build host), but that key has a
passphrase, so password auth is what actually gets used today.

## Partition layout

| Partition | Node | Holds |
|---|---|---|
| `BOOT` | `/dev/sda10` | ztOS uniLoader + mainline kernel + initramfs; current device image is kernel #27, with PCIe/Wi-Fi resume support |
| `RECOVERY` | `/dev/sda11` | stock TWRP 3.7.0_9-0, byte-identical to `stock/RECOVERY.img` |
| `USERDATA` | `/dev/sda25` | the Arch root, ext4, label `ZR_ARCH` |

Everything else, `EFS` included, is untouched. `stock/` holds verified dumps of
BOOT, RECOVERY, EFS, CPEFS, RADIO, DTB, DTBO, PARAM, MISC and the rest, each
compared against the device after reading.

## Getting back in when something breaks

Nothing here can leave the phone unbootable, and every layer has a way back:

1. **Arch does not come up** → the initramfs drops into a rescue shell with the
   same USB network: `telnet 172.16.42.1`, or `scripts/psh.py 'command'`.
2. **The kernel hangs** → uniLoader's failsafe has already armed the cluster-1
   watchdog (30 s) and written `INFORM3 = recovery` into the PMU, so the reset
   comes up in TWRP, where `adb` works.
3. **Nothing on USB at all** → the boot stage's deadman resets the phone 600 s
   after boot unless `/run/zboot.hold` exists (the USB gadget unit creates it,
   so a healthy boot disarms it).
4. **TWRP itself is gone** → Download mode (Volume Down + Bixby + Power) is in
   the bootloader and cannot be overwritten; `stock/` has everything to put
   back with heimdall.

Arch is on `BOOT` and TWRP on `RECOVERY`, so: a normal power-on or
`systemctl reboot` lands in Arch, `reboot recovery` (or Volume Up + Bixby +
Power) lands in TWRP, and from TWRP a plain `adb reboot` comes back to Arch.
All three were tested on the device.

## Scripts

Everything runs unprivileged on the build host; nothing needs sudo.

| | |
|---|---|
| `scripts/build-boot.sh <name> [--fallback boot\|recovery] [--power] [--touch] [--cpufreq] [--gpu] [--decon] [--wlan] [--no-ufs] [--cmdline …]` | kernel + initramfs + uniLoader + Samsung boot image → `out/<name>.img` |
| `scripts/build-arch-root.sh [size]` | Arch Linux ARM tarball → ext4 image labelled `ZR_ARCH` (fakeroot + `mke2fs -d`, no loop mount, no chroot) |
| `scripts/flash.sh BOOT\|RECOVERY <image>` | write a boot image from TWRP over adb, with readback verification |
| `scripts/flash-boot-from-arch.sh <image>` | write BOOT from the running Arch system, over the USB link |
| `scripts/pacman-proxy.py` | HTTP proxy on the host so the phone's pacman can reach a mirror over USB |
| `scripts/flash-from-rescue.sh <image>` | the same, from our own rescue shell when TWRP is not running |
| `scripts/flash-root.sh [image]` | write the root image into `USERDATA` in verified, resumable 256 MiB chunks |
| `scripts/psh.py 'command'` | run one command in the rescue shell (telnet over USB) |
| `scripts/ssh-pw.py <user> <pw[,pw2]> <host> 'command'` | ssh with a password and `su`, without sshpass |
| `kernel/make-dts.py` | derive the starlte device tree from the S9+ one; every edit is asserted |
| `kernel/uniloader-failsafe.py` | patch the watchdog + fallback-target writes into uniLoader's first board hook |

Build inputs live in `build/` (kernel tree, toolchain, busybox, the ALARM
tarball) and are not in the repository. The ALARM tarball's GPG signature was
verified against the Arch Linux ARM build key `68B3 537F 39A3 13B3 E574 D067
7719 3F15 2BDB E6A6`.

## Build switches

Each block that can hang the bus when it is read while gated is a switch, off
unless asked for, and the device tree and the kernel config are flipped
together:

    scripts/build-boot.sh zboot-full2 --fallback recovery \
        --power --touch --cpufreq --gpu --decon --wlan

## What differs from the S9+ port it is built on

`kernel/make-dts.py` turns `exynos9810-star2lte.dts` into the starlte tree and
fails loudly if an upstream change moves anything it edits:

* 4 GB of RAM, so the memory bank at `0x9_00000000` is removed (the S9+ has one
  more);
* battery: 3000 mAh, 4.35 V float, 1.5 A charge current (stock values);
* model and compatible strings;
* the panels the spec's phase 2 would need (GPU, touch, PMIC) start disabled and
  are switches, not edits;
* `--fallback` picks what the kernel arms as the next boot target before it
  touches anything risky.

## Wi-Fi

The radio works and scans; it needs its password once, on the phone:

    sudo zboot-wifi 'ZeroneLABS WIFI' '<password>'

That writes a hashed `wpa_supplicant-wlp1s0.conf`, enables
`wpa_supplicant@wlp1s0` and lets `systemd-networkd` take the DHCP lease
(`20-wlan.network`). The firmware is the phone's own, lifted from its VENDOR
partition — `nvram.txt_r02a_b2` names itself *"BCM4361B2 … for STAR1 KR"* — and
is built into the kernel image, so nothing is loaded from the root filesystem.

### With a desktop: NetworkManager owns Wi-Fi

Plasma's (and phosh's) Wi-Fi settings talk to NetworkManager, so with only
`wpa_supplicant@wlp1s0` running the radio is connected but invisible in the UI.
On the phone as it stands today Wi-Fi has been handed over:

    mv /etc/systemd/network/20-wlan.network{,.disabled} && networkctl reload
    systemctl disable --now wpa_supplicant@wlp1s0
    systemctl enable --now NetworkManager

`/etc/NetworkManager/conf.d/10-zboot.conf` keeps `usb0` unmanaged: the USB link
to the build host stays with systemd-networkd, whatever NetworkManager does.
A 64-hex PSK from the old `wpa_supplicant` file can be reused as
`wifi-sec.psk`, so the password need not be typed again. `zboot-wifi` is the
path for an image without a desktop.

The default target is `graphical.target`; `zboot-session
plasma-mobile|phosh|hyprland|gnome` picks what greetd starts and restarts the
session cleanly. Two things greetd needs for that, both set on the phone by
hand (greetd is not part of the image build):

* `/etc/pam.d/greetd` carries `session optional pam_systemd.so type=wayland`
  before the `system-local-login` include. Without it the logind session is
  type `tty` and mutter exits with "Failed to find any matching session".
* `zboot-session` removes `/run/greetd.run` before restarting greetd, so the
  restart runs `[initial_session]` (a `user` class session) again rather than
  `[default_session]` (a `greeter` class one).

`gnome` is stock GNOME Shell 51; the mobile fork is not packaged for Arch
Linux ARM.

## ztMETRO

`ztmetro/` is our own compositor and shell: one C file on wlroots 0.20, in the
manner of Windows Phone 8.1. Start, the status bar, the nav bar and the
on-screen keyboard are drawn by the compositor itself; one app at a time fills
the space between the bars.

    scripts/ztmetro-push.sh --start     # copy to the phone, build there, switch to it
    zboot-session ztmetro               # on the phone, as root

| Gesture | Does |
|---|---|
| tap a tile | launch the app, or return to it if it is running |
| drag on Start | scroll the tiles |
| nav bar: back | Escape to the app; long press: the next running app |
| nav bar: Start (the Arch logo) | back to Start; the app keeps running |
| nav bar: search | show or hide the keyboard, until search exists |
| left edge, swipe right | close the app |

**Rendering** is its own OpenGL ES 2 quad renderer on the context wlroots
creates: every tile, bar, key sheet and app surface is a textured quad with a
4x4 model matrix under one perspective projection, drawn straight into the
output's swapchain buffer. Frames are rendered only while something is damaged
or animating (0 % CPU idle, about 90 MB). On top of it: tiles lean toward the
finger, Start swings away round the left edge on launch (staggered, the tapped
tile last) and the app swings in, the reverse on the way back, and the
keyboard slides.

**Keyboard**: Windows Phone layout, an Esc / Tab / Ctrl / Alt / arrows row for
terminals (by `app_id`), a symbols page. Keys are real key presses from a
virtual `wlr_keyboard`, so terminals work; `text-input-v3` shows and hides it
for apps that ask.

**Action centre**: pulled down from the status bar, it covers everything above
the nav bar: the status row with the charge and the date under it, eight quick
actions four across (each its own quad, leaning under the finger like a tile),
CLEAR ALL and COLLAPSE, the notifications, and a bar in the accent colour to
push it back up by. Its contents do not travel: they sit at the top and the
bar comes down under the finger uncovering them, while what is behind goes
dim. It is only redrawn when a toggle changes. Wi-Fi and airplane mode go through `nmcli`; rotation lock,
flashlight, location and brightness only keep their state, as the kernel has
nothing behind them yet, and there is no notification service.

**Motion** is timed off a 30 fps recording of a Lumia (`build/ref-video/`).
Start leaves in 340 ms, the tiles furthest from the top left first and the
tapped one alone at the end, each easing in and out, then 80 ms of black. ztMETRO's own apps (`app_id` starting
`zt`) are told when the tile was tapped (`ZTMETRO_LAUNCH_MS`) and bring their
own entrance; anything else swings in whole.

**Apps wider than the screen**: Firefox will not go narrower than about 450
points. A window that comes out wider than the 360 it was asked for is drawn
smaller to fit, asked to be taller by as much, and touches are mapped back.

**ZrnMetroCore** (`ztmetro/core/`): the window, frame clock, scrolling, page
turns, loading dots and widgets that ztMETRO's own apps share. An app gives it
a function that draws a page (`zmc_header`, `zmc_entry`, `zmc_switch`,
`zmc_slider`, `zmc_field`, `zmc_choice`) and callbacks for taps, sliders, back
and "this page has arrived"; `zmc_watch_fd` and `zmc_every` put its own file
descriptors and one timer in the main loop, `zmc_busy` runs the dots. The
header file opens with a whole app in a dozen lines. `ztsettings` and
`zrndevtest` are built on it.

**ZrnDevTest** (`ztmetro/devtest/`, tile `</>`): system state, the readings
the kernel exposes refreshed each second while that page is open, the systemd
journal, kernel log and shell log as they are written, and a list of what
hardware this kernel drives and what it does not.

**zrnperfd** (`ztmetro/perfd/`, `zrnperfd.service`): a minimal take on the
laptop's daemon. Root, asleep in `read()` on `/run/zrnperfd.ctl`; lines
`profile powersave|balanced|performance`, `deepsleep on|off`,
`screen off|on`. State in `/run/zrnperfd.status`. Set from Settings >
performance; the shell tells it when the screen goes off and on. Screen off
holds both clusters and the GPU at their lowest operating points (measured:
350 MHz, 400 MHz, 260 MHz). **Never take a core offline on this kernel: it
cannot be brought back without a reboot** (`CPUn: failed to come online`).

**Settings**: Wi-Fi is asked of NetworkManager in the background with the
loading dots running; new pages *performance* and *developer tests* (demo
tiles on Start, slow motion x5, show touches, lock screen on or off: config
keys `demo_tiles`, `slow_motion`, `show_touches`, `lock`).

**Sound**: the speakers work, through `snd-zrn-abox` (see `audio/README.md`):
Samsung's audio DSP firmware started from a loadable module, one ALSA playback
device, PipeWire on top. Volume is capped below stock because speaker
protection is not ported. No earpiece, headphones or microphones yet.

**Volume keys** drop a banner with the level out of 30 and set the PipeWire
sink's volume (`wpctl`).

**Charging**: `zrn-charge` (udev, and once at boot) reads the charger type from
the MUIC and raises the MAX77705's limits to 1.5 A for a wall charger; the
kernel driver alone stays at 500 mA. No 9 V fast charge.

**Idle and ambient light**: the shell provides `ext-idle-notify-v1` and
starts `hypridle` if installed (`~/.config/hypr/hypridle.conf`), which runs
`~/AutoBrightnessLinux` (third_party/AutoBrightnessLinux, with a ztMETRO
backend added in `core/ztmetro_backend.py`) on idle and on resume. That
program now sets brightness and colour temperature through the control pipe
(`brightness N`, `tone KELVIN`; the shell multiplies every pixel, as there is
no gamma table). It reads the hub's light sensor through
`ALS_IIO_ROOT=/run/zrn-sensors` in its `.env`; with no camera OpenTone stays
off, and `python main.py --set-tone 4000` works by hand.

**App switcher**: hold the Start button (which now reads ZRN). The open apps become a row of cards
at 0.6 size, the one in front in the middle and older ones to its left. Swipe
along the row, tap a card to bring it forward, throw one up off the screen to
close it and end its process; any nav button leaves. Cards carry the app's `app_id`.

**Wi-Fi glyph**: bands light by signal (-60, -72, -82 dBm), read with `iw`
when the status row is drawn anyway; nothing polls.

**Fonts**: Segoe WP is used when installed (drop the TTFs in
`/usr/local/share/fonts/` on the phone and run `fc-cache`), Selawik otherwise.
It is Microsoft's and is not shipped or fetched here.

**Automatic brightness** follows the light sensor behind the phone's sensor
hub: `zrnsensord` (see [sensors/README.md](sensors/README.md)) turns lux into
`brightness N` for the shell, with no polling, and stops listening while the
screen is off. Settings > brightness has the switch.

**Lock screen**: shown at start and whenever the screen comes back on: the
status row, the time, the day and the date. Push it up a fifth of the screen to
unlock; a tap makes it hop. There is no PIN.

**Brightness** is done in the shell, as there is no backlight device: a black
quad over everything, up to 85 % opaque, which on this OLED panel is a dimmer
screen. The settings slider and the quick action both drive it.

**Key popups**: a letter or symbol key under the finger is drawn again, larger
and in the accent colour, just above itself.

**Power**: the battery glyph carries a plug while any supply other than the
battery is online; the kernel's power-supply uevents trigger the redraw. The
screen sleeps after `timeout` seconds without input (60 by default, 0 for
never) and the power key sleeps and wakes it.

**Checking the look without touching the screen**: `ztmetro --render DIR` and
`ztsettings --render PAGE FILE` draw into PNGs with no display at all.

**Settings app**: `ztmetro/settings/ztsettings.c`, a plain Wayland client
(wl_shm, cairo, pango, touch; no toolkit) in the look of the phone's settings
hub. Going into an entry, the rows turn away round the left edge top to bottom
30 ms apart, the tapped one last; the screen is black for 80 ms; the new page's
rows turn in the same way (half resolution while it moves, about 750 ms in
all). Going back, both pages turn from the bottom up. The title stays put and
the list scrolls under it from a cached drawing: it coasts when let go, pulls
past either end at 0.4 of the finger and springs back. Sliders follow the
finger. The hub's rows arrive like that at launch and leave like that on close. It writes `~/.config/ztmetro/config` and sends the
compositor SIGHUP, which re-reads it and redraws on the spot. Back (Escape)
goes up a page, and quits from the first one. Not built yet: entering a
password for a new network, brightness (this kernel exposes no backlight
device), date and time, sound.
`/etc/polkit-1/rules.d/50-zboot-nm.rules` (set on the phone by hand) lets the
wheel group manage networks without a password prompt.

**Config**: tiles in `~/.config/ztmetro/tiles` (`size | colour | label | icon |
command`), or whichever of a built-in list is installed; `accent`, `haptics`,
`columns` (medium tiles across Start: 2, 3 or 4; default 3) and `scale` in
`~/.config/ztmetro/config`. Font: Selawik, installed by hand
under `/usr/local/share/fonts/selawik`. The log of the current run is
`~/.local/state/ztmetro.log`.

**Testing without a finger**: `$XDG_RUNTIME_DIR/ztmetro.ctl` takes `tap X Y`,
`down ID X Y`, `move ID X Y`, `up ID` and `state`, in points, through the same
code path as the touchscreen.

Known:

* **Frame rate: 60 fps since `zboot-full2`.** `s9p-decon` used to report every
  page flip one refresh late (it waited for the latch, then armed the event
  for the frame start after it), which capped every compositor at about 28
  fps: 35 ms per frame with 3 ms of rendering. `kernel/patch-decon-flip.py`
  sends the event at the latch. Measured after: 17.0 to 19.5 ms per frame over
  ztMETRO's transitions, with single frames of up to 49 ms still to be chased.
* The compositor runs Mesa with `PAN_MESA_DEBUG=noafbc`: Panfrost's texture
  compression drew one tile as streaks. Apps keep their own setting.
* No haptics: this kernel has no vibrator driver (`no rumble device`).
* Not there yet: task switcher cards, the app list, continuum, live tiles,
  the action centre, input-method commit strings and accent popups, a lock
  screen, screen blanking, XWayland.

### ztOSCoreSensorFramework

`ztmetro/csf/` (`ztoscsf`, `ztoscoresensorframework.service`): the one place
that knows every piece of hardware support this system brings itself. At boot
it loads the modules that have no service of their own (the LED, the vibration
motor) behind a mark in `/var/lib/ztoscsf` (a load that stops the kernel is
not tried again; `ztoscsf retry NAME`), starts any listed unit that is not
running, and then watches them all: units through one `systemctl show` every
20 s, modules in `/sys/module`, session programs in `/proc`, and the kernel
log for faults. Anything that does not start, stops, or is restarted is sent
to ztMETRO as a notification (`notify TITLE|TEXT` on its control pipe: a
banner, and kept in the action centre). The list is
`/etc/ztoscoresensorframework/components`; `ztoscsf status` prints the state.

### Haptics

`haptics/zrn-haptic.ko` drives the MAX77705 motor as a rumble input device.
ztMETRO plays four pulses (tick, tap, click, heavy) for six kinds of touch
(system, nav bar, tiles and lists, sliding and snapping, inside apps,
keyboard); Settings > haptics has the master switch, strength and a switch
per kind (`haptics`, `haptic_strength`, `haptic_nav|tiles|slide|apps|keys` in
the config). Apps ask through ZrnMetroCore's `zmc_haptic()`, which writes
`haptic KIND [GROUP]` to the shell's pipe.

## ztOS SDK

Third-party apps in Python and Qt: [sdk/README.md](sdk/README.md). `ztos-sdk new NAME`
makes one, `ztos-sdk run` tries it on the laptop, and `ztos-sdk deploy` builds a single
aarch64 binary (a launcher with the app zipped onto it), installs it in the alarm user's
home and opens it in the shell. The phone needs `pyside6 qt6-wayland qt6-svg`
(`ztos-sdk doctor --fix`).

## Hyprland by touch

`zboot-session hyprland` starts Hyprland 0.56.2 with a touch setup; the files
are in `rootfs/home-alarm/.config/` (`hypr/hyprland.lua`, `waybar/`, `fuzzel/`)
and are copied to `/home/alarm/.config/` by hand.

| Gesture / button | Does |
|---|---|
| bottom edge, swipe up — or **Apps** | fuzzel launcher |
| left / right edge, swipe in | previous / next window (scrolling layout, one window per screen) |
| bottom edge, swipe sideways | switch workspace |
| three fingers, swipe down — or **X** | close the window |
| **Kbd** | show / hide squeekboard (`zboot-osk`) |

Gestures are [hyprgrass](https://github.com/horriblename/hyprgrass), built on
the phone in `/home/alarm/hyprgrass` at commit `8e605468`, the one its
`hyprpm.toml` pins for Hyprland 0.56.2 (`meson setup build && ninja -C build`;
needs `glm`). The Arch Linux ARM `hyprland` package ships no `hyprpm`, so the
plugin is loaded by path with `hl.plugin.load()` and has to be rebuilt by hand
after a Hyprland update. Hyprland 0.56 reads `hyprland.lua`; the earlier
`hyprland.conf` is kept as `.bak`.

## Known issue: GPU job faults leave black patches

Under Plasma Mobile the kernel logs a Panfrost fault every few minutes
(`DATA_INVALID_FAULT`, `JOB_BUS_FAULT`, `Unhandled Page fault ... VA 0`, all
from plasmashell's `QSGRenderThread`). Each one leaves a black or garbled patch
that stays until the session restarts. Count them with:

    dmesg | grep -c -E "js fault|Unhandled Page fault|gpu sched timeout"

Rendering *is* accelerated (KWin reports `Mali-G72 MC18 (Panfrost)`, OpenGL
3.1); this is the same fault class the S9+ port documents. Tried so far, all
runtime-only and gone after a reboot:

| Setting | Result |
|---|---|
| `panfrost` `s9p_flush_reduction=N`, `s9p_g3d_pd` `always_on=Y`, devfreq pinned to 572 MHz | one more fault within about a minute |
| the above with devfreq pinned to 455 MHz (950 mV kept) | three more faults in about four minutes of use: no help, reverted |
| `PAN_MESA_DEBUG=sync` in `/etc/environment`, everything else at defaults | set on the phone, result pending |

Not tried yet: `panfrost.transparent_hugepage=0` (needs a new boot image).

Phosh does not start: phoc comes up on Panfrost, then phosh asks for its home
bar as a layer surface of height 0 and phoc ends the connection (`height 0
requested without setting top and bottom anchors`). Pinning the output to
scale 2 in `/etc/phosh/phoc.ini` did not change that.

## Known issue: black spikes drawn by the panel

Separate from the GPU faults above. Jagged black spikes appear near the status
icons and the bottom edge; they are **not** in the frame handed to DECON
(compare a photo of the glass with a grab of the live scanout buffer, read out
of `/dev/mem` at the address in DECON's IDMA register `0x16071040`). Whatever
draws them sits after the framebuffer: DECON, DSC or the panel.

Under test: `s9p-decon` sends DCS sleep-in / sleep-out when the display blanks
and does not carry the panel's power-on sequence. `/etc/tmpfiles.d/zboot-panel.conf`
writes `N` to `/sys/module/s9p_decon/parameters/panel_sleep` at boot, so
blanking only sends display off / on. Whether that removes the spikes is not
confirmed yet. The cost is roughly 150 mW with the screen off.

## pacman without Wi-Fi

The USB link reaches the build host and nothing else. Rather than NAT (which
needs root on the host), run `scripts/pacman-proxy.py` there and use
`zboot-pacman` on the phone, which is `pacman` with `http_proxy` pointed at it.
The proxy is GET-only and refuses anything that is not an Arch Linux ARM mirror.

## Next steps

1. **Wi-Fi password**, as above — then the phone is self-sufficient.
2. **A desktop**: `pacman -S sway foot` or GNOME on Wayland. The port's S9+
   notes say `PAN_MAX_AFBC_PACKING_RATIO=0` is required in `/etc/environment`
   (already set) or glyphs show 16-pixel dashes, and that
   `LIBGL_ALWAYS_SOFTWARE` must not be set anywhere.
3. **The spec's phase 1 proper**: the two-entry menu, `zkeys`, and postmarketOS
   from the microSD card. The boot stage already picks its root by label and
   falls back, so the menu is the missing part.
4. **Power**: no CPU idle states (the S9+ port could not make PSCI
   `CPU_SUSPEND` resume), no suspend. Expect the phone to stay warm and drain
   on battery.
5. **Audio, camera, modem, Bluetooth**: not started anywhere on this SoC.
 
## License

* **ztOS Userspace & Applications** (`ztmetro/`, `os/`, `sdk/`, installer, and system daemons): Licensed under the [GNU General Public License v3.0 or later](LICENSE) (**GPL-3.0-or-later**).
* **Linux Kernel Drivers & Device Trees** (`power/`, `haptics/`, `third_party/`, kernel patches, and DTS files): Licensed under the [GNU General Public License v2.0 only](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html) (**GPL-2.0-only**), conforming to upstream Linux kernel licensing terms.
