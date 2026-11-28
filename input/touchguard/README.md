# zrn-touchguard

Filters ghost touches from a faulty touchscreen. It grabs the touchscreen
(found by name, default "Wacom Multitouch sensor Finger"; the pen is a separate
device and is never touched), creates a uinput clone named
"<name> (zrn-touchguard)" and forwards every frame minus the ghost contacts.
If the daemon dies the grab is released and the raw device works again.

## Rules

- **Stuck point**: a contact that stays within `stuck-mm` (2 mm) for
  `stuck-ms` (8 s) is cut. Its spot (`spot-mm`, 4 mm) is masked while it is
  down and `spot-hold-s` (60 s) after it lifts, doubling each time the same
  spot sticks again, up to `spot-max-s` (30 min). A cut contact that moves
  more than 10 mm is let back in.
- **Noisy band**: a contact shorter than `blip-ms` (60 ms) that moves less than
  `blip-mm` (0.5 mm) is a blip. When `band-blips` (6) blips land within
  `band-mm` (2 mm) of the same row or column in `window-s` (30 s), spread over
  at least `band-spread` (4) different 10 mm cells along it, that band is
  masked for `band-hold-s` (120 s). At most `max-bands` (4) bands at once.
- **Masked areas hold, not drop**: a contact starting in a masked band is let
  through once it lasts longer than a blip or moves `release-mm` (3 mm); in a
  masked spot, once it moves `release-mm`. Real taps there still work, about
  60 ms late; blips never reach the compositor.

## Use

    make && sudo make install PREFIX=/usr/local
    sudo zrn-touchguard --dry-run --verbose     # log only, no grab
    sudo systemctl enable --now zrn-touchguard
    sudo pkill -USR1 zrn-touchguard             # stats to the journal

Options are `--name value` / `--name=value` or `name = value` lines in
`/etc/zrn-touchguard.conf`; `--help` lists them.
