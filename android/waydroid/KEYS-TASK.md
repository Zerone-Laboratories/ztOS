# Waydroid: some ztMETRO keyboard keys don't work (e.g. "@")
Phone access ONLY via `python /tmp/ztos-phone-control.py run|sudo|put|get` (never plain ssh;
two failed logins = stop). Build ztMETRO on the phone in /home/zerone/ztmetro-phase2 (push only
files you change), deploy with backup + rollback (greetd respawns the shell). Other agents work in
ztmetro/browser/ and on Bluetooth: don't touch those.
User: in Android (Waydroid) apps, some keys of the ztMETRO on-screen keyboard don't work, e.g. "@".
ztMETRO's keyboard: ztmetro/KEYBOARD.md, osk code in ztmetro/ztmetro.c (text-input-v3 for
clients that support it, otherwise key events / virtual keyboard with a keymap). Waydroid reads
Wayland key events and maps evdev keycodes through Android's own key layout (US Generic.kl/kcm),
ignoring the Wayland keymap, so any symbol the OSK sends via a custom keymap or a keycode+modifier
combination Android interprets differently breaks.
1. Reproduce: type every symbol on all OSK pages into a Waydroid text field (e.g. WhatsApp or a
   simple Android app), list which fail.
2. Fix at the right place: for Waydroid windows send symbols as US-layout keycode + Shift
   combinations Android's Generic.kcm produces, or deliver text via text-input-v3 commit_string if
   Waydroid supports it (check Waydroid's input method / zwp_text_input), or Waydroid's
   `waydroid shell input text` fallback is NOT acceptable (latency). Keep ATL and native apps
   working (test one of each).
3. Verify all symbols type correctly in Waydroid, ATL and a native app. Write android/waydroid/KEYS.md.
