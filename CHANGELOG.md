# Changelog

What changed in each ztOS release, newest first. The release number is in
`VERSION` (`0.2.3-BETA-1.3`: the GSI's version, then the stage). The ztMETRO
shell has had its own number, in `ztmetro/VERSION`, since the unreleased work
below.

Each release lists its changes under:

- **Added**: something that wasn't there before.
- **Changed**: something that now works differently.
- **Updated**: newer versions, numbers, lists or pins; nothing behaves differently.
- **Fixed**: something that was broken.

Phone details are for the Galaxy S9 (starlte) unless an entry says otherwise.

## Unreleased

### Added
- ztMETRO has a version of its own (`ztmetro/VERSION`, 0.2.3). `ztmetro --version`
  prints it with the wlroots version, and the running shell writes both to
  `$XDG_RUNTIME_DIR/ztmetro.version`.
- Settings > about now reads every value from the phone itself:
  - the phone's name (ztOSIAL) and its model and codename (the device tree);
  - the bootloader, when it passes its version;
  - the ztOS release (`/etc/os-release`), what it's built on and the GSI version;
  - the kernel and its build;
  - the shell that is running and its wlroots version;
  - uptime.
- This changelog.

### Changed
- An image build writes `/etc/os-release` from `VERSION`, so the two can't drift apart.
- While the screen turns, each bar's blur is renewed every fourth frame (#24).

### Fixed
- The 0.2.2 and 0.2.3 images still called themselves 0.2.0-BETA-1 in `os-release`
  and GSI 0.2.1 in ztOSIAL.
- Settings > about showed fixed text ("SM-G960N starlte", "wlroots 0.20") whatever
  the phone or shell.
- The last dropped frame when the screen turns: no frame now takes over 20 ms (#24).

### Updated
- `gsi.json` and `ztos-ial.pc` to 0.2.3.
- The README, the installation guide, ztos.online and devices.ztos.online to 0.2.3 / BETA-1.3.

## [0.2.3-BETA-1.3] - 2026-11-22

### Added
- **Passcode lock**, off until you set one in Settings > passcode:
  - a PIN pad on the lock screen, and emergency calls from it;
  - `zrn-passcoded` keeps your data in an encrypted LUKS2 vault (argon2id), with a
    recovery key in a second slot;
  - each wrong PIN makes the next wait longer, and it never wipes the phone;
  - if you're locked out: `sudo zrn-passcode recover` over SSH.
- Start: small tiles can go in any small cell (medium and wide tiles on even cells).
  Your layout is kept.
- Action centre: pulling it down pushes the screen behind back slightly and darkens it.
- Calls: the call screen opens flat over Start or the app, with the whole background
  blurred at once. Its panels flip in and out with the Lumia 530's call-screen timings.
- Fingerprint groundwork, **built but not loaded**: drivers for the Egistec ET510
  (`zrn-mobicore`, the MobiCore driver ported to Linux 7.1, and a new `zrn-etspi`),
  a device-tree fragment and a test plan in `fingerprint/README.md`.

### Changed
- Smoother shell:
  - the frosted background is blurred into a spare texture, and the two bars are
    blurred on alternate frames while things move;
  - the status row, quick actions, live tiles and the action centre repaint only
    when they change, and never mid-motion;
  - a notification that arrives during a pull waits until the pull ends;
  - list rows are built after the motion ends.
- Battery: Start redraws less while idle (one live tile flip every 12 s, the power
  tile every 5 s).
- Memory: while the screen is off, the shell's own files stay in memory, so waking
  the screen doesn't stall reading them back.
- The app list's letters flip in with the same timings as the call screen.

### Known issues
- The call screen's panel flips still drop 1–5 frames each.
- Passcodes are numeric PINs only.
- Under the DotMatrix style, the lock screen still opens with a swipe.
- Someone with root, or a copy of the storage, could guess a short PIN offline.

## [0.2.2-BETA-1.2] - 2026-11-21

This release also includes 30 earlier commits that had never been pushed.

### Added
- **WhatsApp** (`ztwhatsapp`), installed from the App Store:
  - its own linked-device client (`ztwhatsappd`);
  - pairing, sending and receiving, history, media, status, starred and archived
    chats, group info and search;
  - a Windows Phone Messaging look, and notifications with actions.
- **ZroovMusic**: a music app with its own playback engine, media controls (MPRIS)
  and a live tile. It streams from fast links and from torrents, and has the
  Internet Archive built in.
- App Store:
  - detail pages like Windows Phone's store;
  - ztOS apps listed first;
  - app icons from the signed index;
  - it asks before Remove, and warns that the app's data goes too.
- Store apps run: zrn-sdk is in the image at a pinned commit, with the store's key
  and a source for ztOS apps.
- Turnstile:
  - every tile turns about the screen's left edge, each on its own clock;
  - closing an app plays its launch backwards, bottom row first;
  - reopening a running app plays the whole exit first.
- Calls: the power key silences a ringing call, and volume down rejects it.
- Sleep diagnostics, all off by default: a freeze guard, the analog EINT filter,
  power-domain and FLTCON logging, and a PMIC RTC alarm tool.
- Device list: OnePlus 6T, Galaxy S9 (Snapdragon), Galaxy M20, Xiaomi Note 9 Pro,
  Pixel 3a XL, Lumia 950, Mi A2 Lite and Galaxy S10 5G.

### Changed
- Motion:
  - the CPU clocks are held up during Start, app list, turn and launch animations
    (zrnperfd's motion hold);
  - the shell runs on the efficiency cores, in a cgroup of its own;
  - drawing is batched, and the frosted background is blurred from a half-size copy;
  - apps launch with vfork;
  - the launch animation is slower.
- Sleep: waits for the modem to drop its memory request before the phone sleeps.
- The sensor framework moved from `ztmetro/csf` to `ztmetro/coru` (ztOSCOR-U).
- zram swaps a page at a time (`page-cluster` 0).
- WhatsApp comes from the App Store, not the ztMETRO build.
- ztos.online: the SDK section covers zrn-sdk v2 and links to its wiki.

### Fixed
- After any answered call, the phone ignored every later call. This had been
  broken since BETA-1.
- Release builds had failed since 31 Oct: the build machine was missing Qt's `uic` (#22).

### Updated
- Cross Ports: the phones ztConflo can build for (137–138).

## 0.2.1-BETA-1.1 - 2026-11-01

### Added
- **Calls and messages**:
  - mute, keypad tones in a call, hang-up at once, and the busy and disconnect tones;
  - Phone opens on the keypad, with contacts, history and speed dial as tabs;
  - Messages has Windows Phone 8.1 threads and bubbles, and receives at once.
- **People**: Google contacts (one-way sync) with photos, search, a jump list and
  sticky letters, for thousands of contacts.
- **Camera**: the front and rear cameras (`zrn-cam`), with focus and aperture as
  stock has them. Photos are processed on the GPU, at full size.
- **Microphone**: recording through the CS47L93 codec, with adjustable gain and
  noise suppression. Calls get their microphones and echo reference.
- **GPS** on demand (`ztos-gnss-gate`), with warm starts. Android apps get it as
  their location.
- **Waydroid (Android apps)**:
  - Google push for Waydroid and ATL apps (`zrn-pushd`);
  - media playing in Android apps shows on their tiles;
  - Android alarms can wake the phone;
  - Android calls ring at the ztOS ringer.
- **ztRetrofit DotMatrix**: a second whole-shell style, chosen per session.
- **ztOSIAL** (the hardware interface layer), and OSIAL Management to see which of
  its interfaces answer.
- Hardware nav keys, and `ztos-touchkeyd` for the Galaxy J5's touch keys.
- Lock screen: the clock rises when a notification arrives. Notifications show
  the app's own icon.
- The action centre's list scrolls.
- Pickers and jump grids use the firmware's Swivel transition.
- `ztpower` shows battery drain since the phone was unplugged.
- `zrn-testmode`: anything switched off for a test comes back by itself.
- devices.ztos.online: a device list anyone can add a port to, reviewed on Discord,
  plus Cross Ports from ztConflo. The first ports added were the Poco X8 Pro and Galaxy J5.
- A desktop dev mode for the shell on x86, and an Arch Linux ARM `flake` dev shell.
- Installer: wallpapers and ringtones from a local media pack.

### Changed
- ztOS is split into repositories: ztos-core, ztos-ial, ztos-build and
  ztos-device-samsung-starlte.
- Shell:
  - sizes itself to the panel;
  - Windows Phone's tilt and curves;
  - 60 fps under a scrolling app;
  - app frames go straight to a display plane when they can.
- Brightness: percent is now perceptual, with a 16 ms eased ramp. Auto brightness
  goes by nits.
- SoC sleep dozes instead of being blocked by background apps, with clear rules for
  what may hold it off or wake it.
- Waydroid: an Android app out of sight is stopped.
- No desktop browsers or Chromium in the image. Settings offers auto brightness only
  on phones with a light sensor.

### Fixed
- The modem didn't boot: now only one Android root runs at a time.
- A watchdog reset or kernel panic now restarts ztOS, not TWRP.
- The sensor hub crashed at every boot.
- Wi-Fi after sleep: the PCIe link now retries training.
- Waydroid: no app would open after closing one.
- Superscan crashed at Google.
- The lock screen's drop lost its last frames.
- SMS receiving is reliable, and a call you dialled is never shown as incoming.
- The microphones survive SoC sleep.

## 0.2.0-BETA-1 - 2026-10-26

### Added
- **ztOSIAL, the GSI and device layers**: ztOS is a generic system image plus a small
  layer per phone, meeting at ztOSIAL. The Galaxy S9 is the first device layer.
- **App Store** (`ztstore`), from the package repositories and the AUR.
- **Superscan**, a browser (WPE WebKit) with Windows Phone 8.1's Internet Explorer
  look, video, and frames drawn by the GPU.
- **Phone**: the Windows Phone 8.1 layout and dialer, from the Lumia 530.
- Windows Phone transitions from the Lumia 530 firmware, in the shell and the SDK.
  Toasts swivel in, live tiles flip, tiles have real Windows Phone glyphs, and
  there's a chime at start-up.
- Devices and Services, and buttons and sliders in ZrnMetroCore.
- Background photos: Lumia wallpapers by series, and a picker.
- Hardware: Bluetooth (BCM4361), the rear flash as a torch, panel brightness as a
  backlight device, and hardware video decoding (MFC).
- SoC sleep while the screen is off and nothing plays (`zrn-sleepd`). The modem and
  Bluetooth can wake the phone.
- The ztOS wiki.

### Changed
- S9 brightness uses the panel's own 512 levels, down to 1 %.
- zrnperfd: screen-off idle modes, a battery-first clock ladder and touch boost.
- The project moved to the Zerone-Laboratories organisation.

### Fixed
- Brightness: correct colours, no freezes while dragging the slider, and the panel
  keeps its timing signal after sleep.
- Bluetooth survives sleep.
- The phone stopped polling a dead modem every two minutes all night.
- `make install` refuses programs built for another machine.

## [0.1.0-ALPHA_2] - 2026-10-16

### Added
- A centred Windows Phone 8.1 charging battery icon.
- The ABOX audio firmware and the PipeWire audio stack.

### Changed
- Default animation speed 75 %.
- Hardware acceleration and sleep states.

### Fixed
- The Wi-Fi status icon.
- Vibrator, modem, audio and Wi-Fi detection in the hardware test.
- Sound driver start-up, signal bars and Wi-Fi flapping.
- Installer: the prebuilt download option is hidden, and it tells you which keys
  to hold to reboot.

## [0.1.0-ALPHA] - 2026-10-14

### Added
- First release: the ztMETRO shell on Arch Linux ARM, on the Galaxy S9.
- Sound, Wi-Fi from Settings, and the mobile network.
- Auto brightness from colour light sensors.
- Power: the panel sleeps when blanked, and Wi-Fi switches off after 5 minutes with
  the screen off on battery.
- The installer and automatic builds.

[0.2.3-BETA-1.3]: https://github.com/Zerone-Laboratories/ztOS/releases/tag/v0.2.3-BETA-1.3
[0.2.2-BETA-1.2]: https://github.com/Zerone-Laboratories/ztOS/releases/tag/v0.2.2-BETA-1.2
[0.1.0-ALPHA_2]: https://github.com/Zerone-Laboratories/ztOS/releases/tag/v0.1.0-ALPHA_2
[0.1.0-ALPHA]: https://github.com/Zerone-Laboratories/ztOS/releases/tag/v0.1.0-ALPHA
