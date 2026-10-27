# Haptics fix

## Why it felt bad

Our driver ran the motor through the kernel's `ff-memless`. Reading it against the stock S9 driver
(`drivers/motor/sec_haptic.c`, `max77705_haptic.c`, `exynos9810-starlte_motor.dtsi` in
xxmustafacooTR/exynos-linux-stable; same numbers as `stock/twrp-live.dts` `haptic {}`):

* ff-memless times a pulse in jiffies (HZ=250, 4 ms steps) from a timer and starts the motor from a
  work item. A 7 ms key tick came out 4 to 16 ms long, never the same twice.
* The motor is an LRA (159.5 Hz, period 48828 ns, duty 70 % normal, 95 % overdrive) with a mass on
  a spring. Driven flat it needs about 15 to 25 ms to reach amplitude and rings for 30 ms or more
  after it stops. Short ticks barely started, then smeared: weak and buzzy. Stock avoids that with
  overdrive at the start and a negative intensity (duty mirrored about one half, which drives the
  motor the other way) to brake. Ours had neither.
* The ring vibration was one 38 ms "heavy" tap every 1.8 s. That is why it was bad.
* Stock also has 7 frequency tables (120 to 200 Hz) for its haptic engine; we only use the main one
  (160 Hz, the motor's resonance), which is right for taps.

## Driver (haptics/zrn_haptic_main.c, no more ff-memless)

* One hard hrtimer plays a pulse in three parts: overdrive (od_ms at od_boost % strength), sustain
  (to the requested length), brake (brake_ms of reversed drive). Only timer registers are touched in
  the timer, so timing is microseconds, not jiffies. An RT kthread does the I2C enable; the chip
  stays enabled idle_ms after a pulse, so the next pulse starts with no I2C.
* Measured on the phone (sysfs `stats`): start latency 0 to 0.5 ms; drive time exact (7 ms asked:
  7.0 ms od, 3.5 ms brake; 300 ms asked: 300.0 ms drive); stop from userspace brakes at once.
* Tunable live, no reload: `/sys/module/zrn_haptic*/parameters/{od_ms,od_boost,brake_ms,brake_pct,idle_ms}`
  (defaults 8, 150, 10, 70, 300). `cat /sys/bus/platform/devices/max77705-haptic/stats` shows the last 8 pulses.
  If taps feel too hard raise nothing: lower `od_boost` (100 = no overdrive) or `brake_pct`.
* `Makefile` now also builds `zrn-haptic-dev.ko` (same source, driver name `zrn-haptic-dev`) to
  try a build next to the loaded one. `ff-memless.c` is no longer used (left in the tree).
* Build: `make -C build/linux-7.1/out M=$PWD/haptics ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu-
  modules` with `build/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin` on PATH.

## Ring vibration patterns

* `ztmetro/core/zrnvibe.h` (header only, shared). Patterns: short, long, double, burst, heartbeat,
  rising, match ringtone, off. A child process plays them as one rumble effect per pulse; SIGTERM
  brakes and exits. Even if killed hard, the motor stops within one pulse.
* "match" reads the ringtone wav (16-bit PCM) at runtime: 10 ms loudness, one pulse per note (a dip
  below half the local loudness starts a new one), strength follows loudness, pulses no closer than
  130 ms start to start and no longer than 600 ms, loops with the wav's length. No ffmpeg needed.
  Unreadable wav falls back to "long".
* Config: `~/.config/ztmetro/ring-vibration` is now `ring silent pattern` (old two-number files still read; pattern
  defaults to long).
* `ztphone.c`: call_sound() starts the pattern when a call is incoming and restarts it if the pattern or ringtone changes;
  `sound_stop()` also stops it, and every answer, ignore, hangup and call-end path already calls sound_stop().
  The orchestrator's `modem_tell("speaker on/off")` line is kept.

## Settings page (ztsettings.c: only the audio page and its helpers)

"ringtones + sounds" is now four accent-coloured sections: volume (two sliders), ringtone (one picker
box that opens into the list, replacing 30 inline rows), vibration (vibrate when ringing / when silent,
pattern picker, "feel it" preview), system (sound engine). Picking a ringtone previews it (and its
matching vibration, if the pattern is match). Picking a pattern saves it and plays it once; for match
the ringtone plays with it. Leaving the page stops both.

## Installed on the phone

* `/usr/lib/modules/7.1.0-zboot/extra/zrn-haptic.ko` = new driver (old: `zrn-haptic.ko.before-haptics`). It loads at next boot.
* `/usr/local/bin/ztphone`, `/usr/local/bin/ztsettings` (backups `*.before-haptics`). Built in `~/agent-haptics` from the
  laptop's current `ztmetro/` tree (the repo is newer than `~/ztmetro-phase2`), so ztsettings also carries the other
  person's uncommitted wallpaper-worker changes.
* Live state: to test without a reboot I loaded `zrn-haptic-dev.ko` with insmod, unbound the old driver from
  `max77705-haptic` and bound the dev one (no rmmod anywhere). The old module is still loaded but unbound. At next boot
  the new `zrn-haptic.ko` replaces both. Rollback: copy `.before-haptics` back.

## Not done / to test

* No accelerometer on the phone (sensor hub failed), so strength and crispness were not measured, only timing.
  Feel needs the user's hands; tune with the module parameters above.
* The HAPS table in `ztmetro.c` (tick/tap/click/heavy/key lengths and levels) is unchanged; the shell was not restarted.
  The shell finds the new input device on its own (rescans within 3 s of a failed write).
* Settings page was checked with `ztsettings --render` screenshots only (not driven on the live screen); taps on the
  pickers and "feel it" are untested by hand. Pattern playback was tested through a harness using the same header
  (all 8 patterns, start/stop timing, stats).
