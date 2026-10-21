# Installing ztOS

This guide installs **ztOS ALPHA-2**. The release version is recorded in the
repository's `VERSION` file.

ztOS is a phone operating system: the ztMETRO shell on Arch Linux ARM, with
its own apps, an updater and the ztOS SDK. `ztos` builds it and installs it,
either into an emulator on your computer or onto a Samsung Galaxy S9.

```
installer/ztos                      the guided installer (asks for mode, target, and user)
installer/ztos install --mode build-install  build locally and install immediately
installer/ztos install --mode build-store    build locally and store image for later
installer/ztos emulator             start the emulator again later
installer/ztos guide                how installing on the phone goes, step by step
installer/ztos doctor               is this computer ready?
```

> **Default Credentials:**
> - **Username:** `zerone`
> - **Password:** `toor` (has full `sudo` privileges)


## What you need

A Linux computer (x86-64 or ARM), about 15 GB free, and an internet
connection: the build downloads Arch Linux ARM and about 2 GB of packages. On
Arch Linux:

```
sudo pacman -S --needed util-linux libarchive dosfstools mtools e2fsprogs \
    qemu-user-static qemu-user-static-binfmt qemu-system-aarch64 edk2-aarch64 \
    qemu-hw-display-virtio-gpu qemu-hw-display-virtio-gpu-gl \
    qemu-hw-display-virtio-gpu-pci qemu-hw-display-virtio-gpu-pci-gl qemu-ui-gtk qemu-ui-opengl
```

The build never needs `sudo`. It runs in a user namespace, so your account
needs a subordinate ID range in `/etc/subuid` and `/etc/subgid` (most
distributions add one per user). ARM programs run through qemu-user, so a
build takes 30–60 minutes.

Instead of the `pacman` packages above, run the installer from the repository's
Nix development shell. It supplies QEMU, the aarch64 UEFI firmware, and the
image and build tools:

```sh
nix develop
./installer/ztos install --target qemu
```

On x86-64 hosts you still need `qemu-user-static-binfmt` on Arch Linux, or
`boot.binfmt.emulatedSystems = [ "aarch64-linux" ];` on NixOS.

## Trying it in the emulator

```
installer/ztos install --target qemu
```

It asks for a user name, a password, the phone's name and your time zone,
builds ztOS, and writes it to a disk image in `build/ztos/qemu/`. Then it
starts a window shaped like a phone (540×1110).

- **The mouse is your finger.** Click to tap, drag to swipe, hold to hold.
- **Escape** is the phone's back button.
- **The computer's keyboard** types, or use the on-screen one.
- **ssh** into it with `ssh -p 2222 <user>@localhost`.

The emulator has no modem, sensors or battery, so the phone-only parts of
ztOS stay off there.

## Installing on a Galaxy S9 (SM-G960, "starlte")

> **Read the [known quirks](../README.md#known-quirks) first** (the modem and sensor hub sometimes need a pass through TWRP).
>
> **This erases the phone.** Android and everything in the phone's user data
> are replaced. The phone's identity (IMEI and modem calibration, in the EFS
> partitions) is never touched.

```
installer/ztos guide                 read this first
installer/ztos install --target starlte
```

1. **Unlock the bootloader and put TWRP in RECOVERY.** In Android: Settings >
   Developer options > OEM unlocking. Then, from download mode (Volume Down
   + Bixby + Power), flash TWRP for starlte with Odin or Heimdall.
2. **Back up what you want to keep.**
3. **Start TWRP:** power off, then hold Volume Up + Bixby + Power. Plug the
   phone into the computer (`adb devices` shows it as `recovery`).
4. **Run the installer.** It builds ztOS's boot image and system, then:
   - builds the Galaxy S9 boot image with WLAN enabled; the build applies the
     Exynos9810 PCIe and pinctrl resume fixes automatically
   - asks before writing the boot image to BOOT, and reads it back to check it
   - asks you to **type `ERASE`** before writing the system to USERDATA, in
     256 MiB pieces that are each checked (if the cable comes out, run it
     again and it carries on)
   - offers to restart the phone
5. **The first start takes about a minute** while ztOS grows to fill the
   storage. Then the Welcome app shows how ztOS is used.

The phone image arms `zboot-firstboot.service` on every build, including
reused build trees. It expands the ext4 filesystem to fill the existing
USERDATA partition. Failed initialization keeps its marker so the next boot
retries. Check the result with `df -h /`.

If the phone ever fails to start, it falls back to TWRP by itself. Run the
installer again from step 3.

The Wi-Fi resume support is part of the phone kernel/device tree, not a
separate package. A fresh install that rebuilds the boot image includes it.
Choosing to reuse a previously built `out/ztos-boot.img` reuses that image as
is; rebuild it from the current source to include later kernel fixes.

## What you get

| | |
|---|---|
| Shell | ztMETRO, and only ztMETRO: no other desktop is installed |
| First start | Welcome: Start and tiles, getting around (back, the app switcher, the action centre, the keyboard), your apps, making apps |
| Apps | Settings, ZrnDevTest, Phone, Messaging, People, Music (eco; streaming and offline playback), WhatsApp (eco; linked-device text messaging), GNOME Terminal, Firefox, Chromium |
| Updates | Command-line updater (`ztos-update check` / `apply`) and daily check; the Updates application is not preloaded yet |
| Equalizers | Equalizer applications and EasyEffects are not preinstalled |
| Developing | the ztOS SDK is installed (`ztos-sdk`), with its warm runtime started with the session; see `sdk/README.md` |

## How the build works

`installer/ztos_installer/build.py`:

1. Unpacks Arch Linux ARM in a user namespace (root and the system users
   mapped onto your subordinate IDs).
2. Runs ARM `pacman` in a chroot through qemu-user to install the packages in
   `os/packages/`.
3. Builds ztMETRO and its apps from `ztmetro/` inside the system.
4. Adds `os/overlay/` (ztOS's identity, session, updater, default tiles,
   fonts), the target's own overlay, the SDK, and ztOS's apps (built from
   selected projects in `os/apps/` and Music in `apps/music/` with the SDK).
5. Creates your account, turns on ztOS's services, and writes:
   - for the emulator, a GPT disk with systemd-boot and the generic kernel
   - for the phone, the system image for USERDATA (the boot image comes from
     `scripts/build-boot.sh`)
