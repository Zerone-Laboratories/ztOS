# Round-2 sleep holes: the phone benches (30 Oct)

Run on the phone as root, through `systemd-run --unit NAME` (USB SSH drops
during a sleep), from /home/zerone/team-r2-holes. Results are in
power/suspend/README.md, "Sleep holes closed in round 2".

| script | what |
|---|---|
| race.sh N [MAXDELAY] + race-helper.py | R1: a helper waits for the `wakeup_count` step, sleeps a seeded random delay (0-MAXDELAY s, 0.8), renames `timed-r2` into the inhibit directory and writes `r2: renamed` to /dev/kmsg, while `zrn-sleepd once 60 --ignore-inhibit timed-push` runs. A run fails if the rename comes before that cycle's `zrn-system-sleep: handoff`. |
| g1.sh | G1: a GNSS reader streams, `once 60 --ignore-inhibit timed-gnss`; then GPS off and a plain `once 60`; hub PIDs, clear_grp4 and crash lines |
| swap-init.sh | the live system.img swap with init patched (patch-init-wake-alarm.py beside it) |
| a1-check.sh | container init and system_server capabilities, system_server's timer clockids |
| a2loop.sh | `once 1800` again after each modem wake until another wakes it; dumpsys alarm before each |
| j1.sh | GPS off, `once 60` and `once 300`; hub PIDs, first fix, crash lines |
