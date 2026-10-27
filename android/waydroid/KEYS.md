# Waydroid OSK keys

Tested on the starlte phone on 2026-10-24. The ASCII symbol fix is deployed.
Waydroid emoji remain a separate unsupported input path; this is not a claim
that every key on the emoji page now works in Android.

## Cause and fix

The OSK's ASCII table already uses the correct US evdev codes. The old
`vkbd_send` sent Shift only through `wl_keyboard.modifiers`. Waydroid's
composer sends evdev key events to Android, but does not apply that Wayland
modifier state to Android's keyboard. Android therefore saw the unshifted key.

The shell now sends physical modifier presses before a Waydroid key, then
releases the key and modifiers in reverse order. Shift, Ctrl, and Alt are
balanced for each OSK activation. Other clients retain their XKB modifier
path. Waydroid identification uses `waydroid.*` and the `Waydroid` full-UI ID;
ATL does not use that prefix.

For an enabled, focused text-input-v3 editor, printable OSK input now uses
`commit_string` followed by `done`. That includes emoji. There is no duplicate
key event for a committed character. Shortcuts, Backspace, Enter, and navigation
retain key events. Disabled or unfocused text inputs receive no commit.
The installed Waydroid composer exposes `wl_keyboard`, but no text-input or
input-method protocol strings. Its IME maps as `waydroid.InputMethod`; that
window is an existing visibility signal, not a text-input-v3 editor.

No `waydroid shell input text`, subprocess-per-key injection, Android image
patch, clipboard replacement, or browser/Bluetooth change is used.

## Reproduction

Real shell `tap` commands select the same OSK actions as touchscreen input.
The Android target is the retained Phase 2 probe's ordinary `EditText`.

Before the fix, these 14 keys produced the following characters:

| OSK | Android before | Android after |
| --- | --- | --- |
| `@` | `2` | `@` |
| `#` | `3` | `#` |
| `$` | `4` | `$` |
| `%` | `5` | `%` |
| `&` | `7` | `&` |
| `*` | `8` | `*` |
| `+` | `=` | `+` |
| `(` | `9` | `(` |
| `)` | `0` | `)` |
| `!` | `1` | `!` |
| `"` | `'` | `"` |
| `:` | `;` | `:` |
| `?` | `/` | `?` |
| `_` | `-` | `_` |

Digits, unshifted punctuation, and lowercase input worked. Uppercase input
had the same missing-Shift mechanism. The first eight emoji produced no text;
Android's Generic.kl does not map the OSK's F13 custom-map scan codes.

## Verification

`tests/test_keys.py` compiles the actual shell functions into focused harnesses.
It checks 720 key/modifier/client combinations, balanced modifier releases,
unchanged native/ATL event sequences, focused text commits, Unicode,
shortcut bypass, editing-key fallback, and disabled/unfocused editors.

```sh
python android/waydroid/tests/test_keys.py
```

`tests/verify_keys_phone.py` captures each OSK row separately on the 360x740
phone. It rejects runs if focus changes or the keyboard disappears. Native
results are compared with the fixture's exact UTF-8 value. ATL results can be
compared with the GTK client's returned `set_surrounding_text` values using
`KEYS_WAYLAND_LOG` and a probe launched with `WAYLAND_DEBUG=client`.

The native fixture is `tests/keys_native.c`, built on the phone against the
existing ZrnMetroCore sources. It writes only to `KEYS_VALUE_PATH`; no contacts,
messages, or account data are used. ATL uses the retained probe APK directly,
with the normal Wayland/GLES environment, without changing its app registry.

Expected symbol rows are `1234567890`, `@#$%&*-+()`, `!"':;/?_=`, and `,. `.
Letter checks use `qwe` followed by `QWE` to check that Shift releases correctly.
All 24 emoji are checked in three rows in native and ATL clients.

Results from real input (screenshots and logs in [results/keys](results/keys/)):

| Client | Digits and every ASCII symbol | Lowercase/uppercase | All 24 emoji |
| --- | --- | --- | --- |
| Waydroid probe, modifier-only build | Pass | Pass | Unsupported; see limitation below |
| ATL probe | Pass, exact returned text | Pass, exact returned text | Pass, exact returned text |
| ZrnMetroCore native fixture, modifier-only build | Pass, exact UTF-8 | Pass, exact UTF-8 | Pass, exact UTF-8 |

The final build passed the complete ATL checks. The final Waydroid rerun was
interrupted by an external Firefox launch and subsequent GPU allocation
failures; `keys-waydroid-final.log` records the rejected run. It does not
replace the successful modifier-build screenshots. The native fixture passed
before the text-input addition; it does not advertise that protocol, and the
focused harness verifies the unchanged fallback. Final phone revalidation
requires exclusive control. The task is incomplete for Waydroid emoji.

Waydroid emoji cannot be represented by US Generic.kcm key combinations.
The OSK currently maps emoji onto custom XKB symbols at F13 and later evdev
codes. Android ignores those XKB assignments. Completing that path requires
Unicode-capable integration with Android's editor; the ASCII modifier fix
cannot supply supplementary Unicode characters. Native and ATL emoji remain
available. Do not interpret the Waydroid ASCII pass as an emoji pass.

Some runs lost focus when another controller opened Power Management,
Developer Test, or Firefox. Those captures do not prove keyboard behavior. Keep the
phone idle during reproduction; the driver now fails on that interference.

## Deployment and rollback

Only changed files were pushed. `ztmetro` was built in
`/home/zerone/ztmetro-phase2` with `make ztmetro`. Compilation completed before
the final atomic install. Build output is `keys-build.log` there. Existing
warnings were retained. greetd respawned the shell after installation.

Final deployed SHA256:

```text
064ebbbf37155aa270733ec76f12f3b4d97ed7731b430a2304c1b2325033cdd7
```

Original binary backup: `/usr/local/bin/ztmetro.keys-backup`, SHA256:

```text
45025513b6c2d295cb16160bc3fedcd34c7b99b21de54db944b2c8204cc57a5d
```

From the repo, rollback through the required phone relay:

```sh
python /tmp/ztos-phone-control.py sudo 'sh /home/zerone/ztmetro-phase2/rollback_keys.sh 064ebbbf37155aa270733ec76f12f3b4d97ed7731b430a2304c1b2325033cdd7'
```

The rollback script refuses to overwrite a later deployment. Restarting the
shell disconnects clients; the next normal Android launch replaces its stale
Wayland session. Allow approximately 50 seconds for this phone's shell startup
before unlocking and launching tests.
