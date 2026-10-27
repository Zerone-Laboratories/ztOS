# Task: WhatsApp in both runtimes (two tiles)

Read android/waydroid/PHASE2.md (constraints apply: ztMETRO backups + rollback, no user-data
deletion, do not break ATL). Phone access: `python /tmp/ztos-phone-control.py run|sudo|put|get`;
two failed logins = stop. Unlock before screenshots via /run/user/1000/ztmetro.ctl:
`screen on`, `down 8 180 610`, `move 8 180 450`, `move 8 180 200`, `up 8` (0.15 s apart).
Look at every PNG before claiming anything.

User decision: keep BOTH WhatsApp tiles — the existing ATL one ("WhatsApp") and a new Waydroid
one ("WhatsApp (Waydroid)"). Only one can be logged in per number; that is accepted.

1. Extend the ztos-android registry (applications.json) and CLI so one package can be
   installed in both runtimes: entries keyed by (package, runtime). Keep old registry files
   working (migrate in place, backup first). `launch PKG` keeps working when only one runtime
   has it; add `launch PKG --runtime atl|waydroid` and use it in the .desktop Exec lines when
   a package is in both. `uninstall PKG [--runtime ...]`, `list` show both rows.
2. .desktop files: distinct file names per runtime, labels "WhatsApp" (atl) and
   "WhatsApp (Waydroid)". Same icon is fine.
3. ztMETRO: the Waydroid window (app_id waydroid.com.whatsapp) must map to the Waydroid tile,
   the ATL window to the ATL tile — check how ztmetro.c matches windows to tiles/recents and fix
   if both resolve to the same entry. Keep backup and verify the shell restarts.
4. Install /home/zerone/.local/share/ztos/android/apks/com.whatsapp.apk with
   `--runtime waydroid`. Waydroid already has old /data/data/com.whatsapp data from an
   earlier test: do NOT clear it. Do NOT register/verify a phone number or send messages.
5. Verify by tapping each tile via ztmetro.ctl: both open (Waydroid copy shows whatever its
   first screen is), app list shows both tiles, recents shows them separately, ATL WhatsApp still
   opens its chats. Screenshots in android/waydroid/results/whatsapp-dual-*.png.
Append results to android/waydroid/PHASE2.md.
