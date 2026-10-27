# Phone redesign implementation report

## Delivered

The phone UI and modem path now support the WP 8.1 incoming, outgoing, active, and ended call screens. `ztmetro.c` draws the phone over the lock layer and routes locked-screen touch events to it. Ending a call returns to the lock screen. Keypad and call-screen transitions use ZrnMetroCore's swivel transitions.

The phone plays incoming ringtones in a loop with configured vibration, outgoing `ringback.wav`, DTMF key sounds, and busy, drop, and disconnect sounds. Settings has a **ringtones + sounds** page with ringtone preview and selection. Nokia Tune is the default. Call audio routing remains unavailable; the active-call page says so.

The modem has `fake_ring NUMBER`, `fake_dial NUMBER`, `fake_answer`, `fake_end`, `fake_busy`, and `fake_drop` control verbs. They exercise call UI paths without placing calls. Hutch's LTE-only CS registration fallback sets mask `0x03` after 60 seconds and logs the change.

Third-party and OS Sound Kit audio remains in the git-ignored `os/overlay/usr/local/share/sounds/ztos/phone/` directory. Nokia Tune source: [Nokia Lumia 800 ringtone MP3](https://zvukitop.com/wp-content/uploads/2021/03/nokia-tune-lumia-800.mp3), converted with ffmpeg to mono 44.1 kHz PCM WAV. The phone has 70 phone sound files, including `Ring01`–`Ring21`, WP8 variants, Nokia Tune, call sounds, ringback, and DTMF sounds. `ringback.wav` was added because the outgoing-call code expected that exact name.

## Build and phone verification

Built `ztmetro`, `settings/ztsettings`, `phone/ztphone`, and `modemd/ztoscorenetworkingd` in the phone's isolated `~/swarm-phone` copy. Installed binaries after backing up the prior versions as `.before-phone`; restarted the modem service and greetd under `/tmp/swarm-shell.lock`. The services came back active.

Verified fake incoming calls while unlocked and locked. On the locked screen, answering opened the active-call view; keypad opened and accepted a DTMF key; ending returned to the lock screen. Fake outgoing showed the calling state. Fake calls ended with `call=idle` and `call_result=ended`. Settings' audio page rendered with Nokia Tune selected. The modem journal recorded `Hutch CS absent on LTE for 60 s; WCDMA/GSM mask 0x03`.

Screenshots captured from the phone and visually checked:

- [Incoming call, locked](evidence/live-incoming-locked.png)
- [Incoming call, unlocked](evidence/live-incoming-unlocked.png)
- [Active call, locked](evidence/live-active-locked.png)
- [Active call keypad](evidence/live-active-keypad.png)
- [Outgoing call](evidence/live-outgoing.png)
- [Ringtones and sounds settings](evidence/live-ringtones.png)

## Remaining

Call audio is not routed to the earpiece or microphone. This is an existing platform limitation; real calls were not placed. The fake incoming vibration path was exercised in code, but its motor pattern was not independently measured.

## Polish follow-up, 2026-10-25

`ztmetro.c` now fills the call overlay's margins with the call page's 0.10 grey
and draws status glyphs above the call. The status row uses that same background
while the call overlay is active, then returns to black on call end or unmap.
The phone client already paints the correct background and needed no change.

Verified on the phone after connectivity returned. The settings/icons agent had
already incorporated this shared source fix in its installed shell. Its build
source and the current repository source both hash to
`45908e9a8b6c308f3942dc365fef05093651ed70e4c57cd78dce085c2bf9c740`.
A fresh isolated shell build also passed; no additional shell restart was needed.

Before reference: `evidence/live-incoming-locked.png` (black margins, no status).
Fresh after evidence: `evidence/polish-call-after.png`,
`evidence/polish-call-active-after.png`, and
`evidence/polish-call-ended-lock-after.png`. Fake incoming, touch-to-answer,
and fake end worked while locked; ending restored the lock screen. Status
icons remain visible over uniform grey call margins. No real call was placed.
