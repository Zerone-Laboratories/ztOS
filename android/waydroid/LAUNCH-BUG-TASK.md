# Bug: Waydroid app does not start when tapped in ztMETRO

Read android/waydroid/PHASE2.md and PHASE2-TASK.md (constraints apply: keep ztMETRO backups,
roll back on failure, no user-data deletion, do not break ATL). Phone access:
`python /tmp/ztos-phone-control.py run|sudo|put|get`; two failed logins = stop.
Unlock before screenshots: write to /run/user/1000/ztmetro.ctl
`screen on`, `down 8 180 610`, `move 8 180 450`, `move 8 180 200`, `up 8` (0.15 s apart).
Look at every PNG before claiming anything.

User report: installed Instagram with --runtime waydroid; tapping it in the app list does
nothing. Evidence:
- ~/.local/state/ztmetro.log: `launched 'ztos-android launch com.instagram.android' as 131534`
  (02:54:25), no `showing app 'waydroid.com.instagram.android'` until much later; a window
  with app_id 'Waydroid' appeared at 03:03:28.
- Launching the same command over SSH (unlocked screen, session RUNNING, container FROZEN)
  works: Instagram login shows within ~6 s.
Find the real cause: reproduce exactly the ztMETRO tap path (spawned by ztMETRO, its env,
no TTY, from each starting state: session stopped, container frozen, session running) and
capture ztos-android/waydroid output (make ztos-android log to
~/.local/state/ztos-android.log if it does not). Suspects: ztos-android prompting or blocking
without a TTY, sudo needing a password in ztMETRO's env, cold session start timing out,
unfreeze racing `waydroid app launch`, the window arriving as app_id 'Waydroid' (not
waydroid.<pkg>) and not being shown/mapped to the tile, Instagram's first-run window.
Fix it so a tap always shows the app (show it as soon as its window maps; cold start should
give visible feedback while the container boots). Verify by tapping the tile via ztmetro.ctl
(not SSH launch) from each starting state, with screenshots. Also tap WhatsApp (ATL) once to
confirm ATL still launches. Append results to android/waydroid/PHASE2.md.
