# Boot splash

From the bootloader until the shell's splash is dismissed, the screen shows
the ztOS logo and, below it, a live tree of what systemd is starting now.
There is no text log anywhere in a normal boot.

```
                 ztOS

INIT
├── ztoscoru.service
│   └── ztoscoru
│       └── modprobe zrn-haptic
├── ztoscsf-sensorhub.service           12 s
│   └── ztoscsf-sensorhub
│       └── sleep 0.5
├── ztos-gnss-lto.service             failed
└── NetworkManager.service
    └── NetworkManager --no-daemon
```

| Stage | Covers | Who draws | Where |
|---|---|---|---|
| 2 | uniLoader until systemd (0 to about 2.6 s): the logo, no tree | uniLoader, then initramfs/init | a new boot image: [STAGE2.md](STAGE2.md) |
| 1 | systemd's first batch (about 3.0 s) until the shell becomes DRM master (about 12 s): logo and tree | `ztos-splash --boot` on `/dev/fb0` | `ztmetro/splash/`, `ztos-splash.service` |
| 1.5 | the shell's own splash (about 12 to 27 s): logo and the same tree | the shell, from `/run/ztos-splash/tree` | ztmetro (another agent); the file comes from `ztos-splash` |

Measured on the S9 (boot of 2026-11-19, stage 1 only):

- the first frame came 527 ms after the unit started, at 3.04 s
- the hand-over came at 12.02 s, when systemd-logind became DRM master for the
  shell's session
- a cold first frame (caches dropped) takes about 1.2 to 1.4 s

## Two phases

`ztos-splash --boot` runs from systemd's first batch:

- `DefaultDependencies=no`
- `WantedBy=sysinit.target`
- ordered after nothing; no unit waits for it

**Drawing.** From the start until the shell has the display:

- the program owns `/dev/fb0` (fbdev mmap; it never opens `/dev/dri`)
- it unbinds fbcon and records that in `/run/ztos-splash.state`
- it paints the logo once, exactly as the shell's `draw_splash` draws it
  (font, size, the shell's scale, centring), in the user's theme (`theme`
  and `style` from the greetd user's `~/.config/ztmetro/config`)
- it repaints only the tree rows that changed, at most every 125 ms

**Hand-over.** The program checks every 250 ms. It hands over when either of
these is true:

- `/sys/kernel/debug/dri/*/clients` shows another process as DRM master
  (systemd-logind, which takes the device for the shell's session)
- a new `/run/user/<uid>/ztmetro.ctl` appears

At the hand-over:

- the program closes fb0 and never writes to it again
- it deletes the state file, so the console is never rebound under the shell
- it goes on publishing

**Publishing.** The tree goes on being written to `/run/ztos-splash/tree`
(see below), until `/run/ztos-splash/done` appears or the lifetime ends. The
program publishes from its very first moment, so the file is there before
the shell starts.

### Why the hand-over cannot draw over the shell

An fbdev write is only visible while the fbdev buffer is the scanout. When
the compositor commits its first frame, s9p-decon scans the compositor's
buffer, and nothing written to fb0 can appear over it.

The program stops at logind's DRM master, which comes before the shell's
first commit (backend start, then output, then render). So the last thing on
the panel before the shell's first frame is the program's full picture:
logo plus tree, frozen at that moment.

### "It goes blank and comes back"

This is not the splash. The likely cause is in the display driver:
`s9p_decon_pipe_disable` pushes a black frame, then `set_display_off` and
`enter_sleep_mode` (panel down). `s9p_decon_pipe_enable` then wakes the panel
(panel up, 40 ms, panel on).

Any commit from the compositor that is a full modeset (the CRTC disabled and
enabled again) blanks the panel for the length of that sequence.

To check:

```sh
journalctl -k -b -o short-monotonic | grep -iE 's9p|decon|panel'
```

Look around the hand-over at about 12 s.

To fix it, either:

- the shell or wlroots avoids the modeset (same mode, no disable), or
- s9p skips the blank in `pipe_disable` when the CRTC stays active in the
  same commit (a kernel change)

## Ways out

| When | Drawing (we own the screen) | Publishing (the shell owns it) |
|---|---|---|
| `/run/ztos-splash/done` | n/a | exit |
| `--timeout 120`: no shell after 120 s | console back, exit | n/a |
| `--stall 60`: no job change in 60 s | console back, exit | ignored (the shell is up) |
| `--yield zboot-charge-hold.service:6`: low battery, it talks on the console | console back, exit | n/a |
| `--lifetime 300` | console back, exit | exit |
| SIGTERM (stop, shutdown, emergency/rescue conflicts, `RuntimeMaxSec=330`) | console back, exit | exit |
| crash or SIGKILL | `ExecStopPost=ztos-splash --restore` rebinds, as the state file says | the state file is already gone: nothing |

The publishing phase never rebinds the console and never draws.

On every exit, the tree file is removed. systemd also removes
`/run/ztos-splash` (`RuntimeDirectory=`) when the unit stops.

### The connection to systemd

The program talks to PID 1 over `/run/systemd/private` from PID 1's first
moment; the bus is not needed.

| Event | Effect on the connection |
|---|---|
| `daemon-reload` | keeps the connection |
| `daemon-reexec` | resets it ("Connection reset by peer") |

Both were tested on the laptop with the user manager's private socket.

- **Lost connection:** the program connects again at once, lists the jobs
  again, and says nothing until it exits ("reconnected to systemd N time(s),
  last: <error>").
- **Job signals after a reconnect:** PID 1 sends job signals to every private
  connection without a subscription. The program also subscribes again on
  every connect. Jobs keep their IDs across a re-exec. Each `ListJobs`
  rebuilds the tree, which repairs anything missed while disconnected.
- **The reconnect at 10.8 s on the phone:** `zrn-crash-apply` runs
  `systemctl daemon-reload`, which does not drop the connection. Something
  else did; the next boot's log names the error.
  `journalctl -b _PID=1 | grep -iE 'Reexecuting|Reloading'` shows which.

## The tree file: `/run/ztos-splash/tree`

**Directory.** `/run/ztos-splash`:

- mode 0755, owned by the greetd user, so the shell can create `done`
- created by systemd (`RuntimeDirectory=`) and by the program
- removed when the unit stops

**Writes.** The file is written to `tree.tmp`, then `rename()`d over `tree`.
This happens only when the content changes, at most every 125 ms. A unit
running 5 s or more changes once a second, because its seconds change.

**Watching.** inotify `IN_MOVED_TO` on the directory, or re-read it when its
mtime changes. When the file is missing, draw the logo alone.

**Format.** UTF-8. The first line:

```
geometry <x_pt> <y_pt> <row_pt> <font_pt> <rows_max> <font family...>
```

`y_pt` is the top of row 0, `row_pt` is the pitch, and `rows_max` is the
number of rows that fit. Today:

```
geometry 24.000 427.000 13.000 9.000 22 DejaVu Sans Mono, Noto Sans Mono, Liberation Mono, monospace
```

Then one line per row, at most `rows_max` lines:

```
<depth> <state> <secs> <text>
```

- **depth:** 0 for `INIT`, 1 for a unit, 2 and deeper for its processes
- **state:** `init`, `run` (a unit at depth 1; a process deeper), `done`,
  `failed` or `more`
- **secs:** non-zero for a unit running 5 s or more
- **text:** the rest of the line; it never contains a newline

Example:

```
0 init 0 INIT
1 run 12 ztoscsf-sensorhub.service
2 run 0 ztoscsf-sensorhub
3 run 0 sleep 0.5
1 failed 0 ztos-gnss-lto.service
1 more 0 … 3 more
```

### How the program draws a row

The shell's tree lands on the same pixels if it follows these rules. `s` is
the scale (4.0 on the S9) and `W` is the screen width in points (360).

**Font.**

- `<font family>` at `font_pt` points, through pango-cairo in a cairo context
  scaled by `s`
- `cw` = the logical width of `"0000000000"` / 10
- `th` = its logical height

**Rows.**

- row `i` spans pixels `round((y_pt + i*row_pt)*s)` to
  `round((y_pt + (i+1)*row_pt)*s)`
- `rh` = that height

**Text.**

- at `(x_pt + depth*4*cw, row_top + (row_pt - th)/2)`
- the width is limited to `W - x_pt - that x`, with `PANGO_ELLIPSIZE_END`

**The right-hand label.**

- `"<secs> s"` for `run` with secs > 0, `"failed"` for `failed`
- right-aligned at `W - x_pt`
- the text's width shrinks by the label's width plus `2*cw`

**Guides.**

- line width `lw = max(1, round(0.5*s))` px
- the cell for level `k` (`0 <= k < depth`):
  - `x0 = (x_pt + k*4*cw)*s`
  - `xc = round(x0 + cw*s/2) - lw/2`
  - the arm ends at `round(x0 + 2.75*cw*s)`
  - `ym = rh/2 - lw/2`
- the shapes:
  - `│` is a vertical bar over the whole row height
  - `├──` is the full vertical plus a horizontal bar at `ym` from `xc` to the
    arm end
  - `└──` is a vertical from the top to `ym+lw`, plus the horizontal
- which shape: row `i`'s level `e` (1..depth) continues when the next row
  with depth `<= e` has depth `== e`. The last level is `├──` or `└──`, the
  levels before it `│` or empty.

**Colours.** Greys are given as 0..1.

| | background | `init`, unit `run` | process `run` | `done`, `more`, the seconds | guides | `failed` |
|---|---|---|---|---|---|---|
| dark | 0 | 1 | 0.62 | 0.38 | 0.45 | #E51400 |
| light | 1 | 0 | 0.38 | 0.62 | 0.55 | #E51400 |

**The shell's side.**

- before its first frame, read the file, so that frame already has the tree
- create `/run/ztos-splash/done` when its splash is dismissed

## Install and test (stage 1)

Build and install:

```sh
cd ~/ztmetro && make splash/ztos-splash
sudo install -Dm755 splash/ztos-splash /usr/local/bin/ztos-splash
# the unit, from the repo's rootfs/overlay/etc/systemd/system/
sudo install -Dm644 ztos-splash.service /etc/systemd/system/ztos-splash.service
sudo systemctl daemon-reload && sudo systemctl enable ztos-splash.service
# link: /etc/systemd/system/sysinit.target.wants/ztos-splash.service -> /etc/systemd/system/ztos-splash.service
```

Without touching the screen:

```sh
sudo ztos-splash --png /tmp/live.png                 # the live tree
ztos-splash --demo 7 --png /tmp/d.png                # the recorded boot, 7 s in
ztos-splash --png /tmp/s.png --no-tree               # compare with: ZTMETRO_SCALE=4 ztmetro --render /tmp/r
```

For the shell's side, without a reboot:

```sh
sudo ztos-splash --demo 40 --publish-only            # a moving tree in /run/ztos-splash/tree for 40 s
sudo ztos-splash --publish-only                      # the live tree, until done or 300 s
# as a user: ZTOS_SPLASH_DIR=/tmp/zs ztos-splash --demo 40 --publish-only
```

On the screen, which stops the session (Waydroid must be restarted
afterwards):

```sh
sudo systemctl stop greetd
sudo ztos-splash --demo 30 &
sudo systemctl start greetd
```

During the demo this logs "handed over to ...; publishing the tree".

After a reboot:

```sh
journalctl -b -o short-monotonic -u ztos-splash
```

It should show:

```
drawing on … after N ms
connected to systemd …
handed over to systemd-logind … publishing the tree
done: the shell's splash is over …
```

## Turn it off

- `sudo systemctl mask ztos-splash.service`
- or `sudo touch /etc/ztos/nosplash`, which also turns off stage 2's
  initramfs logo; stage 2 then shows the text console from the root mount on

Text in a running session: `echo 1 | sudo tee /sys/class/vtconsole/vtcon1/bind`
under stage 1. Under stage 2 use `zboot-fb console`, or `ztos-splash --restore`
while the state file exists.

## Options

| Option | Effect |
|---|---|
| `--boot` | the daemon (the unit) |
| `--publish-only` | never touch the screen: only the file (live, or with `--demo`) |
| `--demo S` | the recorded boot for S seconds. With `--png`: the frame at S. |
| `--png FILE` | one frame into a PNG |
| `--raw FILE` | the first frame, without the tree, as XRGB8888 rows: stage 2's picture |
| `--defaults` | ignore the user's config: the shell's default scale (for `--raw`) |
| `--restore` | the text console back, as the state file says |
| `--theme light\|dark` | override the theme |
| `--size WxH` | the size for PNG and raw output |
| `--fb DEV` | the framebuffer device |
| `--user NAME` | take the config of this user, not the greetd user |
| `--no-tree` | the logo alone |
| `--timeout S` | drawing-phase limit (120) |
| `--stall S` | drawing-phase limit (60) |
| `--lifetime S` | limit for both phases (300) |
| `--yield UNIT:S` | give the screen back when UNIT has run S seconds |

Environment variables:

- `ZTOS_SPLASH_DIR` replaces `/run/ztos-splash`
- `ZTOS_SPLASH_USER_BUS=1` reads the user manager (for laptop tests)

The demo's timeline is the S9 boot of 2026-11-19. Its `ztos-gnss-lto`
failure is invented, to show what a failure looks like.
