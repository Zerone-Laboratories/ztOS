# Waydroid health + battery drain audit
Phone access ONLY via `python /tmp/ztos-phone-control.py run|sudo|put|get` (never plain ssh;
two failed logins = stop). Don't delete Android user data. Another agent fixes Bluetooth and one
fixes Superscan at the same time: don't touch bluetooth or ztmetro/browser.
Facts (2026-10-24): com.android.systemui hung and its invisible ANR dialog took all input (fixed
by `settings put global hide_error_dialogs 1` + restarting systemui). Cause most likely:
zrnperfd pulse-froze the whole container (keep_awake=com.whatsapp -> background_waydroid) 9 s in
10 — the orchestrator removed Waydroid pulsing in perfd/tickle.c (deployed). Also RAM-BOOT
(android/waydroid/RAM-BOOT.md) restricted packages/memory; and a framework.jar patch (WA-LINK.md).
User reports battery draining badly.
1. Make `hide_error_dialogs 1` part of the reproducible Waydroid setup (install.sh/config).
2. Verify Android is healthy after 30 min of normal use: no ANRs/crash loops (logcat), systemui
   responsive, WhatsApp input works; revert any RAM-BOOT restriction that causes hangs.
3. Battery: measure screen-off drain for 20 min (battery current_now/charge_counter, wakeups via
   /proc/interrupts deltas, top CPU users incl. Waydroid container cpu.stat, zrnperfd, sleepd):
   with Waydroid kept awake (keep_awake) vs frozen. Report numbers, fix the biggest drains
   (anything polling, Android services busy in the background, the container not idling), keep
   messaging apps working. cpuidle (zrn-cpuidle) must be loaded at every boot (it was skipped
   after today's forced resets — check the mark logic is sane).
Write android/waydroid/HEALTH.md with numbers.
