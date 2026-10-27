# New ztMETRO logo: nav bar, nav icon setting, unlock splash

Phone access ONLY via `python /tmp/ztos-phone-control.py run|sudo|put|get` (never plain ssh;
two failed logins = stop). Unlock before screenshots via /run/user/1000/ztmetro.ctl:
`screen on`, `down 8 180 610`, `move 8 180 450`, `move 8 180 200`, `up 8` (0.15 s apart).
Look at every PNG before claiming anything. Keep the deployed ztMETRO binary as a backup
(/usr/local/bin/ztmetro.logo-backup), verify the shell restarts (greetd respawns on pkill)
and roll back on failure. All UI GPU-drawn, smooth on the four efficiency cores. Another
agent may be testing filesystem recovery on the phone (initramfs/FS-RECOVERY-TASK.md): do
not reboot the phone; coordinate by checking `pgrep -af ztos-fsck` before restarting ztMETRO.
Build/deploy ztMETRO the way previous agents did (see android/waydroid/PHASE2.md deploy notes).

Logo: ztmetro/assets/logo/ztmetro-logo.svg (viewBox 73.65x68.49, white fill, TWO polygons:
left shard `46.47,10.87 0,18.23 0,65.35`, right shard `73.65,68.49 23.23,61.63 73.64,0`).

1. Nav bar: replace the "ZRN" text (ztmetro.c ~5050, draw_text "ZRN") with the logo by
   default, sized/centred to match the back/search glyphs' visual weight in NAV_H, white
   (follow the nav bar's existing colour/theme handling, incl. light themes if any).
   Draw the polygons directly (cairo path from the SVG points or the existing icon pipeline),
   cached — no per-frame SVG parsing.
2. Settings (ztmetro/settings/ztsettings.c): a "navigation bar" entry (in the system or
   applications pivot, wherever display/start settings live) with a picker of nav-bar
   centre icons: the ztMETRO logo (default), "ZRN" text (old), and 2-3 tasteful Metro-style
   alternatives (e.g. a plain Start square, four-pane flag of squares, a circle) — each shown
   as a preview tile. Store the choice in the existing settings/config mechanism ztMETRO
   already reads (find how other shell options like lock/wallpaper are passed: ztmetro.ctl
   keys / config file) and apply live without restart.
3. Boot unlock splash: current boot flow = ztOS splash, lock screen covers it, user unlocks,
   Start tiles fly in (ztmetro.c ~5147 "Hold the ztOS splash..." and the tile in-animation
   TILE_IN_DEG/TILE_IN_DEPTH ~187, ~10000). Change, for the FIRST unlock after each boot only:
   after unlock, show the logo centred as TWO tiles (each polygon is one tile, white on the
   accent/black background as the Start screen would be), animate them IN with the exact same
   tile in-animation (turnstile/flip curve, stagger like two tiles in a row), hold briefly
   (~600 ms), animate them OUT with the exact same tile out-animation used when Start swings
   away, then the normal Start tiles fly in. Later unlocks unchanged. Must not delay input
   longer than ~1.5 s total; a tap during it skips to Start. Respect slow-motion dev setting.
4. Verify on device: screenshots of nav bar (default logo), settings picker, switching to
   another icon and back; for the splash record frames (grim in a loop at ~10 fps, or the
   slow-motion dev setting) proving: two logo tiles in, hold, out, Start tiles in. Log timings.
Write ztmetro/LOGO.md with results.

## CORRECTION from the user (overrides item 3 wording)
The unlock splash looked "broken": it showed the logo as two TILES. Wrong. Show the LOGO
itself: the two white polygons exactly as in the SVG (correct proportions and gap), centred,
on a plain BLACK background — NO tile squares, NO accent fill, NO borders behind the shapes.
Only the motion is borrowed: each polygon (left shard, right shard) moves with the Start
tile in-animation (same 3D turn/depth curve, small stagger), holds, then leaves with the same
tile out-animation, then the Start tiles fly in. Re-verify with frames and look at them.
