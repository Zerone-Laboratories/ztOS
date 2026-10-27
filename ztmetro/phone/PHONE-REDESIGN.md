# Phone app redesign (WP 8.1 look, full-screen incoming call, stock sounds)

Status: designed, not implemented (usage limit reached 2026-10-24).

## References (scratchpad copies were temporary)
- Incoming call, WP 8.1: dark grey screen; small accent caps "INCOMING CALL"; number/name in
  large light type; caller detail line; buttons: "answer" (accent fill) | "ignore" (grey),
  full-width grey "text reply" below.
- Dialer, WP 8: black top half, operator name top-left, number there; 3x4 dark grey keys
  with thin gaps, large digit + small grey letters; outlined "call" / "save" at the bottom.
  ztphone's keypad already matches this; the call page ("in call" pivot + red bar) does not.

## Sounds (OS Sound Kit 1.21, ~/Music/SFX/.../Windows Phone)
dtmf0-9.wav, dtmfast.wav (*), dtmfpd.wav (#), ringback.wav (outgoing, until answered),
Disconnct.wav (hang up), Busy.wav, Calldrop.wav, Callwait.wav, Ring01..21.wma (WP8 folder;
convert to wav/ogg with ffmpeg) -> os/overlay/usr/local/share/sounds/ztos/phone/.
Ringtone loops in ztphone (pw-play) with haptic pattern until answered/declined.

## Animations (zmc_transition, from the Lumia firmware; see devtest PAGE_ANIM)
keypad page in: swivel in (60 to 0 about X, Log 200); call screen in: swivel in / turnstile;
call screen out: swivel out (0.199 s).

## Full-screen incoming call over the lock screen (ztmetro.c)
Today: modemd on RING sends "screen on", "launch ztphone", a notification; ztmetro's
"launch" verb is refused while locked (ztmetro.c ~9618), so a locked phone only notifies.
Plan:
1. New ctl verb "call_screen": launch `ztphone --call-screen` even when locked; set
   server->call_over_lock. modemd sends it instead of "launch ztphone".
2. Only for app_id ztphone while a call exists: draw the shown app AFTER the lock layer
   (render ~10709) and route touches to it instead of ROLE_LOCK (~8761).
3. When that toplevel closes (call over): clear call_over_lock, stay locked.
4. ztphone --call-screen shows only the call page (no history/keypad navigation while locked).

## Call page states (ztphone P_CALL)
incoming: as the reference above (answer/ignore/text reply); outgoing: "calling" + ringback;
active: timer top, name, number; row of square tiles mute | speaker | keypad, then
full-width "end call". Keep the "sound not routed yet" note until call audio exists.

## Related open item
Hutch SIM: calls only register on 2G/3G (no VoLTE). Add to modemd: if CS not registered
within 60 s on LTE, set 080a mask 0x03 (WCDMA+GSM) so calls ring; Settings switch later.
