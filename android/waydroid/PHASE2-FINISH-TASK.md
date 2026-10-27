# Phase 2 finish — deploy, verify, ATL R48 screenshots

Read android/waydroid/PHASE2-TASK.md and android/waydroid/PHASE2.md first (constraints there apply:
keep ztMETRO backup, roll back on failure, no user-data deletion, do not break ATL).
SSH to the phone works again (it was locked out earlier). Use `python /tmp/ztos-phone-control.py`
for access. Do NOT retry auth in a loop: if a login fails twice, stop and report (avoid
faillock lockout).

1. Deploy the Phase 2 fixes that were written but not deployed (diff repo vs device for
   /usr/local/bin/ztos-android, ztMETRO files app, perfd, ztmetro). Back up the device
   copies before replacing.
2. Fix the app registry: the ATL probe may be wrongly recorded as runtime waydroid. Make the
   registry match reality.
3. Verify on the device with grim screenshots (save under android/waydroid/results/):
   install a small APK with --runtime waydroid (find out why the probe never appeared in
   `waydroid app list` and fix), both runtimes' apps in the app list with icons, launch
   each, Back works, keyboard appears once, closing from recents closes the Android task,
   container freezes after leaving and on screen off, zrnperfd boost engages while a
   Waydroid window is focused.
4. ATL R48 (ztOSAndroidRuntimeV1-R48, installed): launch WhatsApp and Instagram via
   `ztos-android launch`, screenshot WhatsApp chat list and one chat (check date/"Today"
   pills and encryption notice render as rounded pills, emoji render, no overlap) and
   Instagram login (no truncated labels, hint text visible). Save to
   android/waydroid/results/atl-r48-*.png and describe what looks wrong, do not fix ATL.
Update android/waydroid/PHASE2.md with results and anything left undone.
