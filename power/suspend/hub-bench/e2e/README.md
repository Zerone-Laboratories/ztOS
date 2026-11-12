# End-to-end bench after round 2 (30 Oct)

Copy to `/home/zerone/team-r2-e2e/` on the phone with
sensors/gnss/test-sleep-e2e.sh, nmea-reader.py, ../r2-reader.py, ../r2-fix.py
and ../holes/race-helper.py, and run each as root through
`systemd-run --unit zrn-sleep-r2-N --collect /bin/bash SCRIPT` (/home is not
executable for a unit; USB SSH drops during each sleep). Results are in
power/suspend/README.md, "End-to-end bench after round 2".

| script | what |
|---|---|
| common.sh | helpers: PIDs, zrn-sleep-init and zrn-bbd counters, hub error counts, Wi-Fi and push timing, sensor freshness after screen on, the next Android wakeup alarm |
| gnss.sh | a reader streams; `once 60`, then up to 15 `once 300` (timed-gnss not counted); GNSS gaps, hub errors, sensors after |
| modem.sh | GPS off, 3 x `once 300`; wake reasons and modem channels, Wi-Fi and push, a 0501 probe answered after |
| race.sh N MAX | the late-inhibit race, N runs, helper delay 0-MAX s |
| alarm.sh | `once 1800` until a wake within 10 s of Android's next wakeup alarm; exit minus due, and zrn-pushd's heartbeat alarm |
| alarmholders.sh | every CLOCK_*_ALARM timerfd: PID, name, time left |
