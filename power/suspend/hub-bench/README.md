# Round-2 bench for the sensor hub through SoC sleep (30 Oct 2026)

Copy these files to `~zerone/team-r2/` on the phone and run them as root
through `systemd-run` (the USB gadget goes down for each sleep):

- `r2bench2.sh off|on SECONDS TAG [FOLLOW]`: one `zrn-sleepd once`. In `on`
  mode a GNSS reader runs through it, and `timed-gnss` is deleted every
  20 ms so the sleep happens (bench only). A cancel caused by the gate's
  rewrite is retried. FOLLOW sets zrn-sleep-init's eint_follow first.
  Results go to `results.txt`.
- `r2seq2.sh "MODE S TAG [FOLLOW]"...`: several runs in a row.
- `r2b3.sh`: B2b4, then the pulse A/B (B3).
- `r2race.sh`: B4, a reader that connects once the step reads `suspend`.
- `r2-reader.py` and `r2-fix.py`: one line for each GGA (boot time and fix
  quality, never a position), and the longest gap and first fix from it.
- `chk.sh`: the load checks after a reboot.

The gate caps screen-off serving at 600 s. Back-to-back `on` runs reach
that cap. Put `/run/ztos-gnss/keep-NAME` in place for the bench, and
remove it afterwards.
