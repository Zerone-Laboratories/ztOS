# Waydroid: first-launch reliability + Home freeze

Read android/waydroid/PHASE2.md (esp. the dual-WhatsApp section near the end) and AUDIO.md.
Phone access ONLY via `python /tmp/ztos-phone-control.py run|sudo|put|get` (never plain ssh). Same constraints as before (ztMETRO backups + rollback, no user-data deletion, do not break
ATL, phone often unattended, no always-on polling, two failed logins = stop). You are the
only agent on the phone now. Unlock before screenshots via /run/user/1000/ztmetro.ctl:
`screen on`, `down 8 180 610`, `move 8 180 450`, `move 8 180 200`, `up 8`. Look at every PNG.

1. Home freeze: the dual-WhatsApp run saw /sys/fs/cgroup/lxc.payload.waydroid/cgroup.freeze
   stay 0 after leaving Waydroid for Home (a concurrent audio-test agent may have caused it;
   now it reads 1). Re-test 5x each: leave a Waydroid app via Home, via Back to Start, via
   recents close, screen off; with keep_awake both off and on (WhatsApp). Record payload
   freeze value 3 s and 30 s later. Fix any path that leaves it running (also check the new
   audio reconnect hook and 90-ztos-waydroid-latency.conf never thaw it).
2. First launch: WhatsApp (Waydroid) first attempt after a compositor restart produced no
   package window; the recovery retry restarted session+container and took 57 s. Find why
   the first attempt failed (~/.local/state/ztos-android.log, waydroid logs, logcat) and fix
   so first attempts succeed. Measure tap-to-map from tile taps (ztmetro.ctl), 5 runs each:
   session stopped, frozen, running, and right after a ztMETRO restart. Target: no retry needed.
Append results to android/waydroid/PHASE2.md.
