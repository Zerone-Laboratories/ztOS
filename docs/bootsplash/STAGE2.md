# Boot splash, stage 2: from uniLoader to systemd

**Goal:** the Samsung logo, then the ztOS screen, with no text at any point.
Stage 1 then takes over without any visible change.

**What it needs:** a new boot image from the workstation tree
(`~/ztos-kernel-waydroid`: `build.sh`, `package.sh`). It changes no kernel
configuration except `CONFIG_CMDLINE`, so the phone's modules stay valid.

**Nothing is applied yet.** Everything is in `stage2/`.

## Where the text comes from today

| Time | Screen | Source |
|---|---|---|
| sboot | Samsung logo | the bootloader |
| uniLoader | **cleared to black, then its ASCII banner and log lines** | uniLoader's `simplefb` driver: `simplefb_probe` runs `clean_fbmem` (memset 0 over 0xcc000000), registers a console, and `print_splash` writes on it |
| kernel, bootfb fb0 registers | Tux, then the log | fbcon takes over at once (`FRAMEBUFFER_CONSOLE=y`, no deferred takeover, `LOGO=y`, `console=tty0 loglevel=7`) |
| decon probes (built in) | "switching to colour dummy device", then fbcon again on decon's fbdev | `aperture_remove_conflicting_devices`, `drm_client_setup` |
| initramfs/init, systemd | `zboot\|` lines, systemd status lines | the console |
| stage 1 (about 3.0 s) | ztOS | `ztos-splash` |

So the owner's "logs before the tree" are uniLoader's banner plus the kernel's
log. Kernel options alone cannot remove uniLoader's text.

## Recommended design

### 1. uniLoader draws the ztOS screen

This is the earliest possible point, about the moment sboot hands over.

`uniloader-splash.py` patches `board/samsung/board-starlte.c` in
`loader-source.tar.gz`:

- **Console off:** `.num_devices = 0`. The simplefb driver is not probed, so
  there is no clear and no text. uniLoader's `printk` then has no screen
  sink.
- **The logo:** in `starlte_init`, right after the write that unlocks the
  framebuffer (`HW_SW_TRIG_CONTROL`), it:
  - fills 0xcc000000 (1440 × 2960 XRGB8888) with the frame's background
  - copies the logo's bounding box (435 × 163 px, 71 KB as one grey byte per
    pixel) from `zboot_logo.h`
- **The header:** `make-logo-header.py` makes `zboot_logo.h` from the same raw
  frame the initramfs paints, and checks that the header gives back that
  frame **pixel for pixel**.
- **Cost:** about the same as today's `clean_fbmem`: one pass over 17 MB,
  now as 32-bit stores.
- **Verified:** built here with the repo's uniLoader, plus the failsafe patch
  and dummy blobs, with the same toolchain as `package.sh`. The binary is
  134 KB.
- **Theme:** uniLoader cannot read the user's theme. It draws the theme
  chosen when the image is built (`--theme`, default dark; the owner's theme
  is dark today). A light-theme user sees dark until init repaints at about
  2.3 s.

**Why not a kernel boot logo:** `CONFIG_LOGO` only draws through fbcon, which
we are switching off. A 1440 × 2960 picture compiled into the kernel would
also be the full 17 MB. uniLoader is earlier, and it is where today's text
comes from anyway.

### 2. The kernel draws nothing

Add to `CONFIG_CMDLINE`:

```
fbcon=map:9 vt.global_cursor_default=0 logo.nologo
```

Remove nothing. `console=tty0 console=ramoops-1 loglevel=7 keep_bootcon
panic=5 …` stay as they are.

- **`fbcon=map:9`:** maps every console to fb9, which never exists, so fbcon
  never takes fb0. That means no clear, no Tux, no text, and no fbdev modeset.
  - The log still goes into tty0's buffer, to ramoops and to `/dev/kmsg` at
    level 7.
  - `FBIOPUT_CON2FBMAP` (`zboot-fb console`, `ztos-splash --restore`) brings
    fbcon back with that whole log.
  - The map also overrides `FRAMEBUFFER_CONSOLE_DETECT_PRIMARY`.
- **`logo.nologo`** and **`vt.global_cursor_default=0`:** no Tux and no
  cursor when the console is mapped back.
- **What stays on the panel:** with no fbcon, nothing commits a frame until
  init. s9p-decon starts by scanning out the inherited 0xcc000000 ("take over
  win=5"). So uniLoader's ztOS picture stays on the panel through the whole
  kernel boot. **This is not measured yet.** If something does blank it, init
  repaints about 2 s in.

Options weighed and not chosen:

| Option | Why not | ABI |
|---|---|---|
| `quiet`, or a lower `loglevel` | The console level is global: ramoops' console log, the record that survives a watchdog reset, would lose everything below it. There is no per-console log level in 7.1 (checked in `kernel/printk`). | n/a |
| `CONFIG_FRAMEBUFFER_CONSOLE_DEFERRED_TAKEOVER=y` | fbcon takes over at the first character printed on tty0, which at `loglevel=7` is at once. Useful only together with `quiet`. | a kernel rebuild with a config change. Probably safe (no exported symbol changes), but unnecessary. |
| `CONFIG_LOGO=n` | `logo.nologo` does the same at run time | **flag:** removes the exported `fb_prepare_logo` / `fb_show_logo`; avoid |
| Dropping `console=tty0` | fbcon still takes the screen. `/dev/console` loses its tty, and the rescue shell's fallback breaks. | n/a |

### 3. initramfs/init

`stage2/init` is the repo's `initramfs/init` plus the block shown in
`stage2/init.patch`. The workstation's init is byte-identical; md5
`d7f0b343c411859e798b872cde00f0cb` is checked before installing.

**First thing after `/proc`, `/sys`, devtmpfs and `/run` are mounted:**

- volume down held (`zboot-fb key 114`) or `zboot.text=1`: the text console
  comes back
- otherwise it paints `/splash-<theme>.raw.gz` in the background (theme from
  `/splash-theme`): `zcat | zboot-fb paint`, which:
  - writes the visible rows (stride and offsets from the fbdev; it waits up to
    1 s for `/dev/fb0`)
  - calls `FBIOPAN_DISPLAY`, which commits decon's fbdev buffer
- The picture is identical to uniLoader's, so nothing changes on the panel.
  From then on, the screen is the fbdev buffer stage 1 draws into.
- This is about 1.8 s after kernel start: the kernel takes about 1.8 s to
  reach init.

**After the root is mounted** (about 2.3 s):

- `/etc/ztos/nosplash` on the root brings the text console back
- otherwise it reads the greetd `initial_session` user's `theme`, `style`,
  `scale` and `display_size`
- if that theme is not the one shown, it repaints in it
- at a scale of the user's own it leaves the picture alone; stage 1 paints its
  own

**Text back** in the rescue shell and when `switch_root` fails.

**`/run/ztos-nosplash`:** whenever init gives the screen to the text console,
it writes this file. `/run` moves into the new root, and
`ztos-splash.service` has `ConditionPathExists=!/run/ztos-nosplash`, so stage
1 leaves the text alone.

**`/dev/fb0`:** init mounts devtmpfs before anything, so it is there with its
kernel numbers (29:0). Nothing needs to be in `initramfs.list` for it.

### 4. Stage 1 takes over

`ztos-splash --boot` finds no fbcon bound. It writes `nofbcon` to the state
file instead of unbinding. It paints the identical frame, so only the `INIT`
row appears. On a failure it maps the console back (`FBIOPUT_CON2FBMAP`)
instead of binding it.

**Resulting sequence:**

1. Samsung logo
2. ztOS (uniLoader, about 0 s)
3. the same frame, held through the kernel
4. the same frame in decon's fbdev (init, about 1.9 s; light users: their
   theme at about 2.3 s)
5. plus the tree (stage 1, about 3.0 s)
6. the shell's logo plus the tree (about 12 s)

## Files (`docs/bootsplash/stage2/`)

| File | What |
|---|---|
| `apply-on-workstation.sh` | Applies it all in `~/ztos-kernel-waydroid`. Idempotent (originals kept once in `.zboot-splash/orig/`; every run starts from them), `--revert`, `--theme dark\|light`, `--no-uniloader`. |
| `zboot-fb.c` | The static initramfs helper: `paint`, `console`, `key CODE` |
| `init`, `init.patch` | The stage 2 init, and its diff against `initramfs/init` |
| `make-logo-header.py` | The raw frame to `zboot_logo.h`, checked pixel for pixel |
| `uniloader-splash.py` | The uniLoader board patch |
| `splash-dark.raw.gz`, `splash-light.raw.gz` | **To make on the phone** (below) |

`apply-on-workstation.sh` makes these changes:

1. **Command line:** appends the three options to `CONFIG_CMDLINE` in
   `linux/out/.config`, and in `kernel.config` if that file has the line. It
   refuses if any other line of either file differs from the original.
2. **Helper:** builds `zboot-fb` with
   `toolchain/…/aarch64-none-linux-gnu-gcc -std=c11 -static -Os -s` (520 KB),
   and checks that it is statically linked. glibc's static libc is fine for
   `open`, `ioctl` and `pwrite`. No libc is needed in the busybox initramfs.
   The phone can build it too, with the same flags.
3. **initramfs.list:** points `/init` to `.zboot-splash/init`, and adds
   `/bin/zboot-fb`, both frames and `/splash-theme`.
4. **uniLoader:** generates `zboot_logo.h`, patches `board-starlte.c` inside
   `loader-source.tar.gz`, and repacks it with the same top-level layout.

Tested here on a fake tree, with the laptop kernel's `.config`, an
initramfs.list in the workstation's format, and a tarball of the repo's
uniLoader:

- apply, apply again (identical result), and revert all worked
- the patched uniLoader compiles

## Steps

On the phone (the fonts must be the shell's):

```sh
ztos-splash --defaults --theme dark  --size 1440x2960 --raw /tmp/splash-dark.raw
ztos-splash --defaults --theme light --size 1440x2960 --raw /tmp/splash-light.raw
gzip -9n /tmp/splash-dark.raw /tmp/splash-light.raw        # about 27 KB each
```

These are exactly stage 1's first frame without the tree, at the default
scale.

On the workstation, with `stage2/` and the two frames copied next to each
other:

```sh
cd ~/ztos-kernel-waydroid
bash ~/stage2/apply-on-workstation.sh --theme dark
bash build.sh && bash package.sh          # detached, as heavy-builds describes
# keep the previous ztos-waydroid-compat.img for rollback
```

To undo: `bash ~/stage2/apply-on-workstation.sh --revert`, then build again.

## Risks on an unattended phone

| Change | Risk | Containment |
|---|---|---|
| uniLoader patch | It runs before the kernel. A fault there means no boot. | The code is a fill plus a bounded copy at fixed addresses inside the 0xcc000000 framebuffer uniLoader already clears today. The zboot failsafe block (INFORM3 fallback target, WDT1 30 s) runs before it **if the workstation's uniLoader has it**: the script prints a note if it does not. Watchdog reset, then the fallback target (TWRP). `--no-uniloader` builds without it. |
| `fbcon=map:9` | **A hang anywhere in the kernel or init shows only the ztOS logo.** No text, no panic message for the 5 s before `panic=5` reboots. A hang looks the same as a slow boot. | `console=ramoops-1` at `loglevel=7` is unchanged: pstore after the reset into TWRP has the whole log. The watchdog (60 s), `panic=5` and the initramfs deadman (600 s) all still reset. USB telnet comes up in init first, and the rescue shell brings the text back. |
| Escape hatches | Every one of them needs a running init. A kernel that hangs before init can only be diagnosed after the fact. | Volume down held at init start (about 1.8 s; hold it once the Samsung logo is up, after sboot's own key combinations). `/etc/ztos/nosplash` (from TWRP or ssh; turns both stages off). `zboot.text=1` (a separate image: the command line is forced). `--revert` and rebuild. |
| Init painting | `zboot-fb` fails, or `/dev/fb0` comes late | It waits up to 1 s, then exit 1 and the boot goes on. uniLoader's picture is still on the panel, and stage 1 paints at about 3 s. |
| Decon without fbcon (not measured) | Something commits a black frame between decon's probe and init | At most about 1 s of black before init's paint. The fix would be in s9p-decon. |
| Theme | uniLoader dark, user light | One dark-to-light change at about 2.3 s |
| Kernel rebuild | Only `CONFIG_CMDLINE` and the initramfs change; the script checks that no other `.config` line moved. Modules keep the same vermagic. | If `build.sh` takes its configuration from somewhere the script does not edit, the command line would silently stay as it was. Check the built image's `/proc/cmdline` after boot. |

## Check on the workstation before running

1. **Where `build.sh` takes `.config` from.** If it copies `kernel.config`
   over `linux/out/.config`, the script edits both. If it uses another file,
   tell me which.
2. **That `build.sh`'s path rewrite does not touch** the new
   `$PWD/.zboot-splash/...` lines in `initramfs.list` (they are already
   workstation paths).
3. **That `loader-source.tar.gz` has `board/samsung/board-starlte.c`** with the
   `HW_SW_TRIG_CONTROL` anchor, and whether it has the zboot failsafe block:

   ```sh
   tar -xzOf loader-source.tar.gz --wildcards '*board-starlte.c' | grep -n 'failsafe\|TRIG'
   ```

4. **Whether `CONFIG_MODVERSIONS` is set** (it is not in the laptop's
   `.config`). It does not matter for a command-line change, but it is worth
   knowing.

## Measure on the phone after the first stage 2 boot

1. Does the ztOS picture stay from uniLoader to stage 1 with no black? Someone
   needs to watch, or film it.
2. `cat /proc/cmdline` contains `fbcon=map:9`.
3. `/run/zboot.log` has `splash: dark logo for zerone`.
4. `journalctl -b -u ztos-splash` says "drawing on …", with no unbind.
5. `ls /sys/class/vtconsole/` shows no frame-buffer vtcon.
6. With volume down held at boot: the text console with the whole log, and
   stage 1 skipped.
7. Before building: does `/proc/bus/input/devices` list a gpio-keys device
   with key 114?
