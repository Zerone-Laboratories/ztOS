# Waydroid: trim Android's RAM, and have it ready at boot

Phone access ONLY via `python /tmp/ztos-phone-control.py run|sudo|put|get` (never plain ssh;
two failed logins = stop). Read android/waydroid/README.md, PHASE2.md, AUDIO.md and
ztmetro/FREEZE.md (the previous agent added 512 MiB zram + memory tuning — build on it, do
not duplicate). Phone is often unattended: every boot must complete by itself. Build ztMETRO
on the phone in /home/zerone/ztmetro-phase2 (push only files you change), deploy with backup
+ rollback. Don't delete Android user data (WhatsApp, Instagram data stay).

## 1. RAM (measured 2026-10-24 ~09:10 UTC)
lxc.payload.waydroid memory.current 1.89 GB. Top Android processes (RSS): WhatsApp 247 MB,
system_server 242, com.android.systemui 181, Instagram 148, surfaceflinger 146,
am.android:fbns (Instagram push) 127, com.android.launcher3 122, plus others.
Phone total 3.5 GB, free 114 MB / available 816 MB while Superscan was open.
- In Waydroid multi-window mode neither Launcher3 nor SystemUI's status bar/nav are shown:
  find what is safe to remove/disable (HOME must still resolve; Phase 1 notes which package
  provides HOME — a tiny stub HOME is fine) and do it reproducibly (strip-image.sh /
  pm disable-user in the installer). Keep IME, PackageInstaller, DocumentsUI, WebView.
- Apps not opened by the user must not run in the background: stop Instagram/fbns
  autostart (pm/appops: RUN_IN_BACKGROUND / standby bucket restricted, or disable receivers),
  except apps in the user's background-apps/keep_awake list.
- Android memory settings for a 3.5 GB host with zram: dalvik heap limits, lmkd (PSI,
  thresholds, `ro.lmk.*`), low_ram already on; cached-app limits (max_cached_processes).
- Target: container idle after boot < 700 MB, with WhatsApp open < 1 GB. Report before/after.

## 2. Ready at boot (user request)
The ztOS splash (ztmetro boot splash -> lock screen; see boot_anim_start / boot_check_sensorhub
in ztmetro/ztmetro.c) must not finish booting until the Waydroid container + session have
started in the background (Android sys.boot_completed=1), so Android apps open fast on the
first launch. Then freeze the container as the idle policy does (it thaws on launch).
- Start waydroid-container + the session early (system/user units) at boot, not lazily.
- ztMETRO holds its splash (with a subtle progress indication if the design has one) until
  Android reports ready, OR a hard timeout (e.g. 45 s) expires — never hang the boot; a
  failed Waydroid must not block the phone (log it, continue, the lazy path still works).
- Measure: boot-to-lock time before/after, first WhatsApp (Waydroid) launch time after
  boot before/after.
Write android/waydroid/RAM-BOOT.md with numbers and changes.
