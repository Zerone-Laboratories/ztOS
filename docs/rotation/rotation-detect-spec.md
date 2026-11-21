# Auto-rotation detection: spec for ztmetro/sensord/ztoscsf-sensord.c

Decision logic lives in the sensor daemon. It streams the accelerometer only while the screen
is on and rotation is on, runs the AOSP WindowOrientationListener algorithm with slightly
faster constants, and tells the shell one line per committed change.

## Facts about the daemon today (verify while reading)
- Clients write `watch accel [MS]` to the FIFO /run/ztoscsf-sensord.ctl; the daemon rewrites
  /run/ztoscsf-sensors/accel ("x y z\n", raw int16, about 4096 LSB per g) per sample; a watch
  expires after 5 s.
- Rate is set by the ADD payload (delay_ms). `sensor_set()` returns early when the sensor is
  already on, so a new period has no effect until REMOVE then ADD.
- `ms` / `wanted` are shared by all users of a sensor: a DevTest watch expiring would stop the
  stream. Rotation needs its own want flag, OR-ed in, never expiring.
- On hub restart (REC_INIT_DONE) sensors are re-added; rotation filter state must reset too.
- `screen off` (from zrnperfd) removes every sensor via sensors_apply(); nothing new is needed
  for sleep. Do NOT enable hub batching and do NOT send new hub commands (the hub asserts).
- The daemon talks to the shell with shell(): non-blocking writes to /run/user/*/ztmetro.ctl
  (e.g. "brightness-auto N"). It keeps its toggles in /var/lib/ztoscsf-sensord/state.

## Frame
Android device frame: x toward the right edge, y toward the top edge, z out of the screen.
Upright portrait: y = +1 g. Face up on a table: z = +1 g. Raw axes are mapped by a signed
permutation `axes` from the config (default "x y z" until measured on the device).

## Algorithm (per accel sample; t = monotonic receive time in ms)
```
a = M * raw
if t - t_last > MAX_GAP or a == (0,0,0): reset(); f = a; t_last = t; return
dt = t - t_last; alpha = dt / (TAU + dt); f += alpha * (a - f); t_last = t
m = |f|
if m < 0.10 g: predicted = NONE; goto propose
if m < MAG_LO or m > MAG_HI: accel_at = t
tilt = deg(asin(f.z / m))                 # +90 face up
push (t, tilt) into a short history
if tilt >= FLAT_DEG continuously for FLAT_MS: flat_at = t
if tilt rose >= SWING_DEG within the last SWING_MS: swing_at = t
overhead = tilt <= OVERHEAD_ENTER ? true : tilt >= OVERHEAD_EXIT ? false : overhead
if overhead or |tilt| > 80: predicted = NONE; goto propose      # keep the current orientation
ang = deg(atan2(f.x, f.y)) mod 360        # 0 top edge up, 90 right edge up, 180 bottom up, 270 left edge up
cand = (round(ang / 90) mod 4) * 90
if cand == 180 and !ALLOW_180: predicted = NONE; goto propose
if tilt outside TILT_TOL[cand]: predicted = NONE; goto propose
if current is set and the angle is not far enough into cand (AOSP isOrientationAngleAcceptable:
   for an adjacent orientation the angle must be at least GAP/2 past the 45-degree midpoint,
   on the side away from `current`): predicted = NONE; goto propose
if cand != predicted: predicted = cand; predicted_at = t
propose:
if predicted == NONE or predicted == current: return
if t - predicted_at < SETTLE: return
if t - flat_at < AFTER_FLAT or t - swing_at < AFTER_SWING or t - accel_at < AFTER_ACCEL: return
if t - committed_at < MIN_INTERVAL: return
current = predicted; committed_at = t
send "orientation <current>" (+ " instant" for the first decision since enable / screen on)
```
Screen on / rotation switched on: reset, seed the filter with the first sample; once
START_SAMPLES consecutive samples agree and all gates pass, send `orientation N instant`.
Flat or overhead at that moment: send nothing.

## Constants (all tunable in /etc/ztoscsf/rotation.conf, key = value; re-read on `rotation reload`)
period_ms 40; tau_ms 150; max_gap_ms 1000; mag_lo 0.59 g; mag_hi 1.41 g; accel_hold_ms 500;
tilt_tol 0:{-25,70} 90:{-25,65} 180:{-25,60} 270:{-25,65}; overhead_enter -40; overhead_exit -15;
flat_deg 80; flat_ms 1000; after_flat_ms 500; swing_deg 20; swing_ms 300; after_swing_ms 300;
gap_deg 45; settle_ms 80; min_interval_ms 600; allow_180 0; start_samples 3; axes "x y z".

## Control and messages
Shell -> daemon (ctl FIFO): `rotation on|off` (remembered in the state file like `auto`),
`rotation reload`, `rotation log on|off` (one journal line per decision and per rejected
candidate with the reason), `rotation record SECONDS` (raw samples with timestamps, plus the
filter outputs, to /run/ztoscsf-sensors/rotation.trace).
Daemon -> shell: `orientation 0|90|180|270[ instant]` (the number names the edge pointing up).
Also keep the current value in /run/ztoscsf-sensors/orientation for a shell that starts later,
and add `rotation=on|off` to the status output.

## Sampling
Accelerometer on when screen_on && rotation on, through a separate want flag; period 40 ms via
REMOVE then ADD in the existing one-transaction queue. Off otherwise. Reset the filter on hub
restart.
