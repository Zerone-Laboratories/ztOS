# Lock screen integration spec: ztmetro.c and zrn-passcoded

This spec is for the agent that owns `ztmetro.c`. Nothing here has been applied yet. Line numbers are from ztmetro.c as of 21 Nov 2026 (22534 lines); search for the quoted code if the lines have moved.

## The rule that comes before everything

**The shell never blocks an unlock on the daemon.** The shell lets the owner in with a plain swipe, and logs `wlr_log(WLR_ERROR, "passcode: ... falling back to swipe")`, in each of these cases:

- `zrn-passcoded` is not running;
- the socket is missing, or the connection is refused;
- the daemon does not answer `status` within 700 ms (`PC_TIMEOUT_STATUS_MS`);
- the daemon answers with anything other than `ok`;
- a `verify` gets no reply within `PC_TIMEOUT_CHECK_MS` (15 s).

A passcode is required **only** when the most recent `status` said `enabled=1`, and that answer is less than 10 s old.

## 1. State (struct zt_server, near `bool lock_enabled;` around line 866)

```c
#include "passcoded/passcode-proto.h"
	/* zrn-passcoded */
	bool pc_required;		/* the last status said enabled=1 */
	uint32_t pc_status_ms;		/* when that status came */
	int pc_kind_pin;		/* 1 PIN (number pad), 0 password (keyboard) */
	int pc_length;			/* the PIN's length: as many dots */
	char pc_typed[72];		/* wiped (explicit_bzero) after every send */
	bool pc_pad;			/* the pad is up over the lock */
	int pc_fd;			/* a verify in flight, -1 none */
	struct wl_event_source *pc_watch;
	long pc_locked_until, pc_skew;	/* the daemon's wait and clock skew */
	int pc_failures;
	char pc_note[96];		/* "wrong PIN", "try again in 0:30" */
	float pc_shake;			/* the dots' shake on a wrong PIN */
```

Initialise `pc_fd = -1`.

## 2. Asking for the status

Call `pc_status_refresh(server)` at two moments:

- **when the lock goes up:** in `lock()` (line ~8409), after `server->locked = true;`;
- **when the screen turns on while locked:** in the screen-on path (line ~14544, `bool locking = ...`).

Use a **non-blocking** `SOCK_SEQPACKET` connection, registered with `wl_event_loop_add_fd`. Never use a blocking `pc_request` on the compositor thread. The reply handler parses `enabled`, `kind`, `length`, `failures`, `locked_until` and `now` with `pc_get`/`pc_get_long`, and sets `pc_required = enabled == 1`. A timer of 700 ms clears `pc_required` (falling back to swipe) if no reply has come by then.

The PIN length is known **before** the owner types, so the dots can be drawn as boxes to fill, as Windows Phone did.

## 3. Where the unlock happens: ROLE_LOCK release (line ~16262)

The success branch is:

```c
if (dm_swiped || (!retro_on(server) && !cancelled && p->py - p->py0 < -server->height / 5.0)) {
	server->locked = false;
	...
```

Change it to:

```c
if (... the same swipe test ...) {
	if (pc_needed(server)) {		/* required, status fresh, daemon answered */
		pc_pad_open(server);		/* the lock rises and the pad comes up: below */
		break;
	}
	server->locked = false;			/* unchanged: the swipe unlock */
	...
```

`pc_needed()` returns `server->pc_required && now_ms() - server->pc_status_ms < 10000`.

**Factor the existing unlock body** (from `server->locked = false;` through `wlr_log(WLR_INFO, "unlocked");`) into a function `lock_open(server)`. The swipe path and a right passcode both call it. **This is the only place that sets `locked = false`** (it was the only one at the time of writing). Check for any other unlock path, such as ztmetro.ctl commands or devtest hooks, and route it through `pc_needed()` too. **Exception:** an incoming call keeps its current behaviour (`call_over_lock`). The call screen may cover the lock, but answering a call must never unlock the phone.

## 4. The PIN pad (Metro style)

- **Layout.** The lock photo stays, moved up by the swipe (`lock_y` animates to about `-height * 0.35`, not off the screen). A dark layer (0.6 black) covers the lower part. On it, top to bottom:
  1. a caption, `enter PIN` (Segoe UI Semilight, 24 pt, lowercase);
  2. a row of `pc_length` boxes that fill in as digits are typed, using the accent colour for filled boxes. For a password, this is a single text box instead, and the shell keyboard comes up;
  3. `pc_note`, in 14 pt, using the accent colour for errors and grey for waits;
  4. a 3×4 pad, the same as in Settings (`settings/passcodepage.h`, `pc_key`): flat filled keys (fg at alpha 0.14), digits in Semilight 30 pt, keys 62 pt tall with 6 pt gaps, and `delete` and `ok` on the bottom row. A pressed key turns the accent colour on press, not on release. Each press plays `ui_sound(UIS_KEY)` and the `HAP_TAP` haptic.
  5. Below the pad, a text button: **`emergency call`**. It is always present, and works even while a wait is running.
- **Draw it on the GPU,** like the other lock layers (`layer_draw`): one `pc_layer` drawn once per change, with no per-frame CPU raster ([[ztosmobile-gpu-everything]]).
- **Under DotMatrix** (`retro_on`), the pad is dots: dot-frame keys, as `zmc_button` draws them. The two styles never mix.
- **Auto submit:** when `strlen(pc_typed) == pc_length`, send `verify` without waiting for `ok`.
- **Back key or swipe down:** the pad closes and the lock drops back. Wipe the typed digits.

## 5. The verify call flow

1. Send `verify\npass=<typed>` with `pc_build` on a non-blocking socket, then immediately `explicit_bzero(pc_typed)`. Show the note `checking...` and the loading dots. Disable the pad until the reply comes. While the check runs, send a `zrnperfd` tickle (`/run/zrnperfd.ctl`) so Argon2 does not run on screen-off clock caps (0.5 s with the boost against more than 2 s without).
2. On the reply `ok`: call `lock_open(server)`, which runs the normal unlock animation and sound.
3. On `error code=wrong`: shake the boxes (a 300 ms damped horizontal shake), play the `HAP_LONG` haptic, and clear the boxes. Set `pc_note`:
   - `wrong PIN`;
   - or, once `failures` ≥ 3, `wrong PIN. N more tries before a wait`.
4. On `error code=locked` (or a `wrong` reply that carries a future `locked_until`): disable the pad (keys dimmed to 40 %). Show `try again in M:SS`, counting down on a 1 s timer from `locked_until - (now + skew)`. **Emergency call stays live.** When the wait ends, re-enable the pad.
5. **Fallbacks** (the rule at the top):
   - On `error code=not-set` (the passcode was turned off meanwhile), or a connection failure, or no reply within 15 s: log the error and call `lock_open(server)`. The owner is never trapped.
   - On a timeout, close the fd so a late reply is ignored.

## 6. Emergency call

The `emergency call` button runs the same command the shell already uses for the call screen:

```c
run_detached("/bin/sh", "-c", "exec env ZTMETRO_NO_ENTRANCE=1 ztphone --emergency", NULL);
```

ztphone would need a `--emergency` mode: a dial pad that allows only emergency numbers (112, 911, 119 and the SIM's ECC list from the modem). That mode is a ztphone task. Until it exists, open `ztphone --call-screen`'s dialer.

Set `call_over_lock = true` so the dialer may cover the lock. **The lock stays locked** underneath the dialer, and closing the dialer returns to the pad.

## 7. Other paths to check

- **Notifications on the lock:** with a passcode set, a tap on a notification opens the pad first. The notification opens only after a right passcode.
- **The shade over the lock:** quick toggles stay usable (as on Windows Phone). Settings and apps launched from it need the passcode.
- **The double-tap wake and the camera shortcut** (if any) must not bypass the pad.
- **`lock_enabled = false`** (the developer setting "Lock screen on wake" off): there is then no lock at all, so the passcode cannot apply. Settings could warn about this when the passcode is on.
- **After an unlock,** keep no secret in memory (`explicit_bzero`). Log only `passcode ok`, `passcode wrong (n)` and `passcode fallback: reason`, never the digits.

## 8. Testing

- Test with a test daemon, never the owner's:
  - start it with `zrn-passcoded --dir /run/zrn-pc-test/state --socket /run/zrn-pc-test/sock --allow-exe <test ztmetro path>`;
  - start the test ztmetro with `ZRN_PASSCODE_SOCKET=/run/zrn-pc-test/sock` (`passcode-proto.h` reads it).
- Cases to cover:
  - the daemon is stopped (swipe works, error logged);
  - the daemon is hung (`kill -STOP`: swipe after 700 ms);
  - a wrong PIN 6 times (wait shown, emergency call works);
  - the daemon restarted during a wait (the wait is still shown);
  - `zrn-passcode recover` while locked (the next status allows the swipe).
