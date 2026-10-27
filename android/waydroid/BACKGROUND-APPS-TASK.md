# Background apps: user-chosen apps keep running (tickled) for notifications

Phone access ONLY via `python /tmp/ztos-phone-control.py run|sudo|put|get` (never plain ssh;
two failed logins = stop). Unlock before screenshots via /run/user/1000/ztmetro.ctl:
`screen on`, `down 8 180 610`, `move 8 180 450`, `move 8 180 200`, `up 8` (0.15 s apart).
Look at every PNG before claiming anything. Constraints: keep ztMETRO/perfd/settings
backups and roll back on failure (greetd respawns the shell); phone often unattended, every
boot must self-recover; all UI GPU-drawn; tune for the four efficiency cores; no always-on
polling; do not break ATL or Waydroid; do not delete user data. Read first:
ztmetro/perfd/tickle.c (header comment), ztmetro/perfd/zrnperfd.c, ztmetro/settings/ztsettings.c
(draw_apps, PAGE_APPS), ztmetro/apps/ztapps.c, rootfs/overlay/usr/local/sbin/zrn-sleepd,
ztmetro/notifyd/ztnotifyd.c, android/waydroid/PHASE2.md (keep_awake, freeze policy).

## Goal (user's words, summarised)
The user can mark ANY app as "Background app" in Settings > applications. Purpose: get
notifications while the screen is off / phone sleeping, since ztOS has no push system yet.
Background apps obey tickle, but closing them (recents/Back) must NOT kill them: they keep
running silently. While the system sleeps, ONE core is dedicated to running them. Tickle for
them is a slow pulse: one wake every ~10 s (not the sub-second pulses of normal tickle).

## Architecture (follow it; report if something is impossible and why)
1. One source of truth: `~/.config/ztos/background-apps` (one app id per line: desktop id /
   ztMETRO app_id, e.g. `firefox`, `ztos-android-com.whatsapp`, `waydroid.com.whatsapp`).
   Settings writes it; ztMETRO, zrnperfd, ztos-android and zrn-sleepd read it (inotify or
   re-read on a control message, no polling). Migrate android.conf `keep_awake` into it and
   keep reading keep_awake for compatibility.
2. Settings > applications: add an "apps" entry/page listing every installed app from
   ztapps (native, ATL, Waydroid — Waydroid entries included, deduplicated as the app list
   does) WITH ICONS (reuse the app list's icon loading/cache; GPU path as the rest of
   settings). Also give the existing entries on that pivot icons if the design calls for it
   (currently none have icons). Tapping an app opens its page with a "Background app"
   on/off toggle (+ short explanation: keeps running for notifications, uses battery).
3. Close behaviour (ztmetro.c switcher kill ~8993 and Back-to-close paths): for a
   background app, hide its window/recents card instead of SIGTERM; for ATL keep the process;
   for Waydroid keep the Android task (move to back, do not `am force-stop`) and do not freeze
   the container for it. Tapping its tile re-shows the existing window instantly.
4. zrnperfd/tickle: background apps get their own "background" tickle class: paused,
   then run in a burst every 10 s (burst >= 1 s so network keepalives/sync can complete;
   make period and burst configurable, defaults 10 s / 1 s). Applies to native/ATL app
   cgroups and to the Waydroid container (lxc.payload.waydroid cgroup.freeze) when any
   Waydroid app is a background app. Non-background apps keep current tickle/freeze rules.
   Audio exemption stays.
5. Sleep: zrn-sleepd. Note memory: Wi-Fi breaks after SoC deep sleep on this phone
   (brcmfmac re-register fails), so deep SoC sleep would kill notifications anyway. While at
   least one background app exists: do not enter deep SoC sleep; instead screen-off low-power
   state with all cores but ONE efficiency core offline (zrn-hotplug; never the last core of
   a cluster rules apply — check memory/README) or, if hotplug is unsafe, a cpuset pinning
   all background-app cgroups + Waydroid container to one efficiency core with the rest idle.
   With no background apps, sleep behaviour is unchanged. Document the battery trade-off.
6. Notifications: verify ATL apps' notifications reach ztnotifyd (check ATL's
   NotificationManager implementation); if not, bridge them. For Waydroid, check for a
   built-in notification forwarder; if none, add a minimal one (e.g. a tiny Android
   NotificationListenerService app installed in the container that forwards to a host
   socket/daemon which calls org.freedesktop.Notifications / ztnotifyd), event-driven.
7. Verify on device with screenshots: settings app list with icons incl. Waydroid apps,
   toggling WhatsApp (ATL) and WhatsApp (Waydroid) as background, closing them from recents
   leaves processes/tasks alive, tickle status shows 10 s pulses (zrnperfd status), screen
   off: one core online / pinned, apps still get CPU every ~10 s, Wi-Fi stays up; a test
   notification from a probe app (ATL and Waydroid) appears in ztMETRO. Measure battery-
   relevant numbers (CPU time per minute of background apps, wakeups) screen off 10 min.
Write android/waydroid/BACKGROUND-APPS.md with design, results and limits.
