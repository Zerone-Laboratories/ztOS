# Tile signals

Live content for an app's Start tile (a "signal"), without a notification. The
tile turns over now and then (a Windows Phone Flip tile) to show it; its front
carries a count, a badge and a thin bar. Under DotMatrix there are no tiles, so
the same signal shows as a short text at the end of the app's row in the app
list, and as a dot on its dock icon.

## Which tile

The signal's **key** is the last element of the tile's command
(`ztsettings`, `com.zerone.music`). For an Android app (Waydroid or ATL), whose
command is `ztos-android launch PACKAGE --runtime ...`, it is the package
(`com.spotify.music`). If no tile has exactly that key, the first whose key
contains it (or is contained in it) is used: a player called `spotify` finds
`com.spotify.music`. An app with no tile on Start shows nothing.

## Fields

| field      | meaning                                                                    |
|------------|----------------------------------------------------------------------------|
| `title`    | caption along the bottom of the back (default: the tile's name)            |
| `line`     | up to three, at the top of the back (after `value`); long ones sweep across |
| `value`    | one large figure at the top of the back                                    |
| `image`    | path of a picture the sender keeps: beside the lines on a wide tile, dimmed behind them on a medium one |
| `progress` | 0..100: a thin bar along the bottom of both faces                          |
| `badge`    | on the front's corner: `play`, `pause`, `bolt`, or a few characters        |
| `count`    | number on the front's corner (0: none); wins over `badge`                  |

Small tiles have no back and keep still; they show `count`, `badge`, `progress`.
A face that changes while its tile is off the screen is drawn when the tile
returns.

## Two ways in

**A file** in `$XDG_RUNTIME_DIR/ztos-live/KEY`, one `field=value` per line,
replaced atomically (write `KEY.new`, rename). It stays until deleted or the
session ends. The shell watches the directory with inotify; nothing polls.

    title=Playing
    line=Wake Me Up
    line=Avicii
    badge=play

From Python: `ztos.tile_update(title, lines, count, value=, image=, progress=, badge=)`
and `ztos.tile_clear()`.

**A line on the control pipe** (`$XDG_RUNTIME_DIR/ztmetro.ctl`), fields apart by
`|` (so no `|` or newline inside a value):

    signal com.spotify.music|title=Playing|line=Song|line=Artist|badge=play
    signal com.spotify.music            (takes it away)

A signal sent down the pipe is kept by the shell through a configuration reload,
but not through a restart of the shell: the sender sends again.

## Who sends what

* **Power Management** (`ztpower`): the shell itself. The battery's draw in
  watts (`value`), charging or on battery (`title`), charge, volts, mA, the
  charge rate in %/h and time to full or left, the battery's temperature.
  Read from `/sys/class/power_supply` every 2 s *only* while the tile (or, under
  DotMatrix, the home) is on the screen and the screen is on, and once when the
  kernel reports a supply change.
* **Linux media players** (`ztnotifyd`): every MPRIS player is a signal keyed by
  its name (`spotify`, `firefox`, an ATL app by its package). Sent when a player
  changes, not on a timer. Album art: a `file://` address is used as it is, a
  web address is fetched once to `$XDG_RUNTIME_DIR/ztos-art/`.
* **ZtOS Music** (`apps/music`): publishes itself with `ztos.tile_update`.
* **Android apps in Waydroid** (`android/waydroid/media/`): a small bridge app
  watches Android's media sessions and reports each change to a host service
  that turns it into a `signal` line keyed by the package.
