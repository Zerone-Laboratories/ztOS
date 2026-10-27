# DemoLoop: a developer-mode demo that loops the shell's best moments

Phone access ONLY via `python /tmp/ztos-phone-control.py run|sudo|put|get` (never plain ssh;
two failed logins = stop). Build ztMETRO and its tools ON THE PHONE in
/home/zerone/ztmetro-phase2 (push changed sources there, `make <target>`); local builds lack
wlroots. IMPORTANT: local ztmetro/ztmetro.c and perfd/ contain half-finished background-apps
edits; the phone's build dir already matches local ztmetro.c, perfd/zrnperfd.c, perfd/tickle.*
— keep it that way (push the files you change, nothing else). Deploy with backup + rollback
(see /tmp/deploy-logo-root.sh on the phone: backup, atomic mv, pkill, verify
"ztMETRO on WAYLAND_DISPLAY" in ~/.local/state/ztmetro.log, roll back on failure). Look at
every screenshot before claiming anything. GPU-drawn UI only; smooth on efficiency cores.

## What the user wants
Settings > developer gets a "DemoLoop" option. When started it runs, over and over:
1. restart the shell (pkill ztmetro; greetd respawns it) -> lock screen
2. unlock -> the boot logo splash (remove /run/user/1000/ztmetro-logo-unlocked first so
   it plays every loop) -> Start tiles fly in
3. Start: showcase the live tiles of Settings and Superscan (dwell until each has visibly
   flipped/updated at least once; if they are not live tiles yet, find what makes them live in
   ztmetro.c and make sure they animate during the dwell)
4. open WhatsApp (Waydroid) from its tile, let it show for a few seconds, close it (Back /
   recents close so the Android task closes, as a user would)
5. open the app list (swipe/arrow to the list)
6. open the file manager (ztmetro/files app) from the list, show it a few seconds
7. go home (Start) -> repeat from 1
Waydroid must be preloaded before the loop starts (session started, WhatsApp warmed up once,
container NOT frozen during the loop — tell zrnperfd/freeze policy to keep it running while
the demo runs, restore normal policy when it stops) so the WhatsApp step is fast.

## Design constraints
- Drive it like a user: through ztmetro.ctl (`screen on`, `tap X Y`, `down/move/up ID X Y`,
  logical 360x740) or add small ctl verbs if positions are unreliable (e.g. `launch DESKTOP_ID`,
  `home`, `applist`, `close_front`). Prefer verbs that act on app ids over hard-coded tile
  coordinates (the Start layout can change).
- A separate runner (e.g. /usr/local/bin/ztos-demoloop, a user systemd unit
  ztos-demoloop.service so it survives the shell restart it causes) runs the loop; Settings
  toggles it (start/stop the unit). Not started at boot; a reboot ends the demo.
- STOP = the user presses VOLUME DOWN three times (within ~2 s). Touches do NOT stop it
  (people will touch a demo phone). While the demo runs, volume-down presses must not
  change the volume. Stopping restores normal policies (Waydroid freeze, screen timeout).
  Also a Stop entry in Settings (reachable after stopping, or if the runner is not active).
- Keep the screen awake while running; the power key turns the screen off as usual and
  pauses the loop (resume when the screen is on again) — it does not stop it.
- Timings at the user's animation speed; each step logs to ~/.local/state/ztos-demoloop.log.
- Phone is often unattended: the loop must never wedge the phone (timeouts per step, give up
  a step and continue; if the shell fails to come back twice, stop the demo).

## Verify
Run 2 full loops on the phone, capture a screenshot at each step (look at them), measure loop
duration and dropped frames (ztmetro.log "animation:" lines), confirm 3x volume down stops it (inject KEY_VOLUMEDOWN via the input path ztMETRO reads, or
verify the handler with a test), touches do not stop it, and that Waydroid freeze policy is restored
after stop. Write ztmetro/DEMOLOOP.md with results.
