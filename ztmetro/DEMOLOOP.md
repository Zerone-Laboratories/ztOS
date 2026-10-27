# DemoLoop

DemoLoop is a developer-mode user service. Settings > Developer starts or stops `ztos-demoloop.service`. The service is disabled at boot, and a reboot ends the demo. The runner survives shell restarts, preloads Waydroid, and keeps its container awake while active. It then restarts ztMETRO, unlocks, shows the Start tiles, opens WhatsApp, closes its task, opens Files from the app list, and returns home.

The `ztmetro.ctl` commands `tile`, `item`, `home`, `applist`, `close_front`, and `flip` drive the UI by app ID or action. The runner logs each step to `~/.local/state/ztos-demoloop.log` and captures screenshots under `~/.local/state/ztos-demoloop/`. Each external command has a timeout. Two consecutive shell recovery failures stop the service.

While the active marker exists, volume-down presses are suppressed and three presses within two seconds stop the user service. Touch input does not stop it. The power key keeps its normal screen toggle behavior; the runner waits while its screen marker says the display is off. Cleanup removes the active markers, restores the configured timeout, and returns Waydroid to its normal power policy. Settings also has a Stop DemoLoop entry.

## Phone verification

Built on the phone in `/home/zerone/ztmetro-phase2`. `make ztmetro check-demoloop-stop` passed. The stop-key test covers three presses within two seconds, timeout reset, and inactive mode. The phone has no usable `uinput` module, so key presses could not be injected. The real key event branch was checked in `ztmetro.c`.

Two complete loops finished. Their logged durations were 92 seconds and 68 seconds. Screenshots from every step were fetched and validated as PNGs. Contact sheets show each captured step: [loop 1](demoloop/evidence/final/loop1-contact.png) and [loop 2](demoloop/evidence/final/loop2-contact.png).

Touch test: a `tap 180 720` left the service active. Pause test: `screen off` set the runner screen marker to `off`; `screen on` resumed it. This exercised the same screen-state path, but did not inject the physical power key. Stop test: stopping the unit removed both markers and restored Waydroid's normal frozen state (`lxc.payload.waydroid/cgroup.freeze=1`). The configured screen timeout was restored to 180 seconds.

The phone log contained 80 animation records during the measured session. Four reported a worst frame above 60 ms; the maximum was 5,703 ms during Android startup. This points to a long stall outside normal shell drawing. The other 76 records had no reported frame above 60 ms.

WhatsApp opened to its first-run welcome screen in loop 2, so no account content appeared. In loop 1 it failed to launch once. Some screenshots also captured transitions before the app list or Start layout finished settling. The runner began loop 3, then stopped during its Settings showcase; two complete loops had already finished. Its cleanup ran and removed the active markers. The stop cause was not established.
