# Bug: host audio (incl. ztOS system sounds) crackles — PipeWire has no realtime priority

Phone access ONLY via `python /tmp/ztos-phone-control.py run|sudo|put|get` (never plain ssh;
two failed logins = stop). Another agent is working on ztMETRO/ztos-android/Waydroid
launch: do NOT edit ztmetro/, ztos-android, or restart ztMETRO/Waydroid. Phone often
unattended: fix must survive reboot and pipewire restarts. Read android/waydroid/AUDIO.md,
audio/README.md, git show 7d7357b 6309064 (audio tuning history).

User: crackling also on ztOS system sounds (host only), not just Waydroid.
Observed now: pipewire (pid 205457, restarted 04:10 UTC after many test restarts today)
threads pipewire, module-rt, data-loop.0 are all SCHED_OTHER (ps cls TS, no rtprio);
ztos-sounds (pid 699) also TS. pipewire.conf has module-rt nice.level -11 rt.prio 88.
`ulimit -r` for zerone = 0; rtkit-daemon.service active. Load avg ~5 (other agent testing).

1. Find why module-rt did not get SCHED_FIFO (journal of pipewire/rtkit, rtkit limits
   e.g. RLIMIT_RTTIME/ max realtime priority 20 vs 88, polkit for rtkit, dbus, the
   pipewire user unit's LimitRTPRIO/ LimitNICE, /etc/security/limits.d). Check whether it
   ever had RT this boot (older journal) — maybe only after restarts.
2. Fix durably: e.g. /etc/security/limits.d/ or a pipewire user-unit drop-in
   (LimitRTPRIO=95, LimitNICE=-19, LimitMEMLOCK) and/or rtkit config so data-loop.0 runs
   SCHED_FIFO after boot and after any `systemctl --user restart pipewire`. Also give
   ztos-sounds' audio thread RT if it has one (via pipewire client RT, not code edits to
   ztmetro/). Check the IRQ thread of the ABOX interrupt (SPI 76) priority as per 6309064.
3. Verify: ps -L shows FIFO for data-loop; pw-top ERR stays flat during 60 s of
   system-sound bursts (requests to /run/user/1000/ztos-sounds.sock) + pw-play music,
   under background CPU load (e.g. 4 busy loops niced 0 on the little cores) — before vs
   after numbers. Restart pipewire twice and re-check.
Append a section "Realtime" to android/waydroid/AUDIO.md.
