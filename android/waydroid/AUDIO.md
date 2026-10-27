# Audio investigation and recovery

Tested on starlte on 2026-10-24, kernel 7.1.0-zboot, PipeWire 1.6.9,
WirePlumber 0.5.18. Changes are installed on the phone.

## Findings

The reported **host-wide audio failure was not reproduced**. Before changing
configuration, `pw-play` opened the ABOX PCM and advanced its hardware pointer.
The initial `closed` PCM, suspended sink and `[init]` system-sound ports were idle
state: the sound helper starts corked and uncorks only when it receives a sound
request. Sending requests to `/run/user/1000/ztos-sounds.sock` produced an
uncorked stream, linked ports and a RUNNING PCM. No sources is expected: this
driver implements speakers only, not the CS47L93 microphones.

A separate, reproducible **Android audio recovery failure** was found:

1. Android AudioTrack playback initially reached the host through its Pulse
   socket and opened the PCM.
2. Restarting `pipewire`, `pipewire-pulse` and `wireplumber` left Android accepting
   all 384000 frames, advancing AudioTrack's position, but creating no host
   playback stream. The PCM remained closed during the test.
3. The socket inode still matched inside and outside the container; this was
   not a stale bind mount in the tested restart.
4. Restarting only `android.hardware.audio.service` restored its host connection,
   active output ports and RUNNING PCM. No driver reload or Android container
   restart was needed. The failure is in the HAL's connection lifecycle after
   losing its host server, rather than ALSA device discovery.

`setprop ctl.restart vendor.audio-hal` was insufficient on this image: it stayed
`stopping`, with the original process alive. Android init logs report failures
creating process groups on its read-only cgroups. Killing only the named HAL
process lets init respawn it; AudioFlinger reconnects. This is the recovery used
below. It briefly interrupts Android audio when the host server changes.

This establishes a cause for Android silence after audio-server restarts. It
does **not** establish that this was the cause of the original host-wide report.

## Crackle

The crackle report reproduced as speaker-sink errors during Android playback.
The Waydroid Pulse stream identified itself as `application.name=Waydroid`,
`media.name=ALSA Playback`, with `application.process.binary=threaded-ml`. It
requested `node.latency=256/48000` (5.3 ms), while the ABOX graph ran at 512
frames (10.7 ms). The stream and graph both used 48 kHz, so rate conversion was
not involved.

With host-only playback, the sink error count stayed flat in stopped and frozen
states. It rose by 5 at the start of the running-idle test, then stayed flat.
With Android AudioTrack playback alone, it rose by 31 on the first run and 32
on the repeat. This isolated the short Waydroid stream buffer as the trigger.

The phone and rootfs overlay now have
`pipewire.conf.d/90-ztos-waydroid-latency.conf`. Its PipeWire node rule matches
only `node.name=Waydroid` and sets `node.latency=2048/48000` (42.7 ms). The
ABOX periods, global quantum limits, and keyboard stream were left unchanged.
The keyboard stream still reports `node.latency=256/48000`.

| Playback state | Before: sink QUANT / client QUANT | Before: sink ERR | After: sink QUANT / client QUANT | After: sink ERR |
| --- | --- | --- | --- | --- |
| Host WAV, container stopped | 512 / 4800 | 240 → 240 | 512 / 4800 | 14 → 14 |
| Host WAV, container frozen | 512 / 4800 | 39 → 39 | 512 / 4800 | 10 → 11 |
| Host WAV, container running idle | 512 / 4800 | 34 → 39, then flat | 512 / 4800 | 10 → 10 |
| Android AudioTrack alone | 512 / 256 | 39 → 70; repeat 70 → 102 | 512 / 2048 | 1 → 2 |
| Host WAV plus Android AudioTrack | not captured before fix | not captured before fix | 512 / 4800, 2048 | 3 → 5 |

The host source was a 30-second, 48 kHz, stereo S16_LE dual-tone WAV. The
Android probe accepted all 384000 frames and reached playback head `384000`.
The combined run kept sink errors to two over 30 seconds; the Android-only run
added one error over its eight-second playback. `pw-top` counters are lifetime
counters for each sink instance, so deltas within each run are the useful
comparison. The graph stayed at 512 frames; only the Waydroid stream latency
changed.

`/proc/asound/card0/pcm0p/sub0/xrun_debug` is absent on this kernel, so no ALSA
xrun count is available. The PCM was observed RUNNING during playback and
closed after host-only runs. These measurements confirm the PipeWire underrun
change; no physical audio recording was made.

## Comparison with the working configuration

- History `6797e5f` introduced the PipeWire stack and ABOX firmware;
  `11bc412` contains the current speaker driver. The installed module and
  `prebuilts/modules/snd-zrn-abox.ko` both hash to
  `aa5c35f32737f5d36d236933a28832e721bd2fbc4b320dca3801d9ffa79e65be`.
- History `6309064` and `7d7357b` configured IRQ scheduling and responsive
  keyboard audio. The live configuration retains 48 kHz, 512-frame ABOX periods,
  eight periods, 256-frame headroom and disabled timer scheduling. No UCM,
  ALSA, PipeWire or WirePlumber configuration changes were needed here.
- The phone's pacman log at 2026-10-23 20:12 UTC shows the Waydroid transaction
  installing only `waydroid (1.6.3-1)`. It did not replace PipeWire, install
  PulseAudio, or install new ALSA/UCM dependencies. `pulseaudio` is absent.
- Phase 1/2 `install.sh`, `network.py`, `recover.sh`, `patch-vendor.sh` and
  session/network units do not replace host audio configuration, sound udev
  rules or audio group membership. The vendor patch changes the composer.
  The session bind-mounts the host Pulse socket. Freeze stops Android execution;
  it does not itself remove the host speaker card.
- `zrn-sleepd` stops PipeWire/WirePlumber and unloads/reloads ABOX around deep
  sleep. Its restore path can exercise the same server-loss boundary. No sleep
  occurred in this boot's sleepd journal; `/sys/power/mem_sleep` offered only
  `[s2idle]` during investigation. No SoC resume failure was demonstrated, and
  forcing deep sleep or reloading the audio module was not justified.

## Fix: event-driven recovery (follow-up)

The original root `ztos-waydroid-audio.service` and its perpetual two-second
Python poll have been removed from the repository and phone. `audio-recover.py`
is now a finite, root-only helper. No recovery daemon, timer or periodic check
runs while idle, including while Android is frozen.

The user unit `ztos-waydroid-audio-reconnect.service` has `After=`, `PartOf=` and
`WantedBy=` relationships with **both** `pipewire.service` and
`pipewire-pulse.service`. It runs `sudo -n ztos-waydroid-recover audio` once per
start/restart transaction, then exits (`Type=oneshot`, no `RemainAfterExit`).
Remaining inactive matters: a later Pulse start must run the check even if a
PipeWire-only start previously deferred it. Global user enablement installs the
two server `.wants` links for subsequent logins/boots. Stopping PipeWire stops
the recovery unit; starting it again pulls the unit in. This covers sleepd's
separate stop/start sequence, not only `systemctl restart`.
`Wants=pipewire-pulse.service` also restores Pulse before reconnecting: sleepd
starts only PipeWire/WirePlumber, and Pulse is stopped through its existing
`BindsTo=pipewire.service` relationship. Without this dependency the first
sleepd-sequence test correctly exposed a deferred reconnect.

`recover.sh` calls the same helper after `prepare`, `unfreeze` and successful
`ready`. These existing launch/thaw paths are outside `ztos-android`; no change
or pending handoff to the other agent is required. Each check compares server
PID/start times, Pulse socket identity and container init PID/start time with
`/run/ztos-waydroid-audio.generation`. An exclusive `/run` lock serializes thaw
and server events. Only a changed generation reconnects the HAL. A frozen or
stopped container is left untouched and its previous generation remains pending;
normal thaw/launch then reconnects it. Repeated thaw without a server change
does not kill the HAL again.

Startup readiness uses at most four checks with 250 ms gaps, only during an
event. External commands have finite timeouts; the user unit has a 30-second
start timeout and no automatic retry timer. Unavailable servers defer recovery
to the next server/launch/thaw event. Failed HAL reconnects do not update the
saved generation. No application data is changed.

Live deployment installed the helper, `recover.sh`, user unit and restricted
sudoers `audio` action, disabled/stopped/deleted the old system unit, reloaded
both managers, globally enabled the new user unit and started it once. The full
Waydroid installer was not rerun. `install.sh` now performs the same transition.
Rollback: disable/stop the new user unit and remove its global `.wants` links;
restore the previous `recover.sh` if removing the helper. Do not restore polling.

## Verification and limits

The following original investigation evidence remains available. Follow-up
measurements for the event-driven implementation are recorded below.

- Direct `aplay -D hw:ZrnAbox,0` of an eight-second 48 kHz, S16_LE stereo WAV:
  RUNNING PCM, advancing hardware pointer and successful completion.
- `pw-play` of that WAV and bursts of system-sound requests: RUNNING PCM and
  linked system-sound ports, including after server restart. Evidence:
  [audio-host-playback.txt](results/audio-host-playback.txt).
- Android framework playback uses the disposable `AudioPlaybackProbe.java`
  through `app_process`, without installing or replacing an application. It
  feeds real AudioTrack samples through AudioFlinger and the Waydroid HAL.
  All 384000 frames were accepted and the playback head reached 384000.
  This tests the Android audio path, not WhatsApp call/media behavior.
- Automatic server-restart recovery: Waydroid output ports `[active]`, PCM
  RUNNING, and both MAX98512 chips report global enable `01`, amplifier `81`,
  source `00`, ADC enable `07`, boost `05/02/01`, and fault flags `00/00/00`.
  Boost measurements were `94`/`95`. Evidence:
  [audio-final.txt](results/audio-final.txt). The read-only I2C diagnostic is
  `tests/audio-amp-status.py`; it does not change amplifier settings.
- Frozen restart: state stayed FROZEN and recovery count stayed 2 before/after
  the server restart. After thaw the count increased and Android PCM ran.
  `pw-top` showed 48 kHz / 512-frame speaker processing and initially zero
  errors; the longer capture includes one error around stream transitions, so
  this is not a claim of universally glitch-free playback. Evidence:
  [audio-frozen-restart.txt](results/audio-frozen-restart.txt).
- Original probe compilation/D8 conversion passed. Follow-up Python compilation
  and `bash -n` checks passed. The new user oneshot is enabled and exits after
  handling an event; an inactive/dead unit with `MainPID=0` is expected.

### Event-driven follow-up verification

Final deployed version passed `tests/verify_audio_events.sh` on the phone:

- Running restart of PipeWire/Pulse/WirePlumber: HAL PID `2657` became `2743`.
  Pulse-only restart then changed it to `2799`.
- Frozen restart: Android remained FROZEN and the saved generation stayed
  unchanged. `ztos-waydroid-recover unfreeze` reconnected it and playback ran.
- The exact sleepd audio-manager sequence (`stop wireplumber.service
  pipewire.service pipewire.socket`, then `start pipewire.socket
  pipewire.service wireplumber.service`) stopped the oneshot and restarted it
  automatically. HAL PID `2830` became `2886`; playback ran. This tests sleepd's
  service boundary without forcing deep sleep or unloading the speaker driver.
- Running-restart, frozen-thaw and sleepd-sequence probes each accepted all
  384000 frames and reached playback head `384000`. PCM was RUNNING, Waydroid
  ports were active, and both amplifiers showed global enable `01`, amplifier
  `81`, source `00`, ADC `07`, boost `05/02/01`, faults `00/00/00` and boost
  measurements `94`/`95`. Captured running graphs used 48 kHz / 512-frame speaker
  processing. Speaker error counters included `5` and `8`; these measurements
  establish recovery, not glitch-free playback.
- Freeze/thaw without a server change kept HAL PID `2886` unchanged.
- During a 20-second running steady-state observation, the recovery unit stayed
  inactive/dead, `MainPID=0`, with no recovery processes in either snapshot.
  Its cumulative CPU usage stayed exactly `687409000` ns. The old system unit
  reported `LoadState=not-found`, inactive; the new user unit was enabled.
  Evidence: [audio-event-driven.txt](results/audio-event-driven.txt).
- A separate ten-second FROZEN observation found no recovery **or probe** process,
  `MainPID=0`, and the same unchanged CPU usage. The test restored RUNNING state.
  Evidence: [audio-event-frozen-idle.txt](results/audio-event-frozen-idle.txt).
  This proves no resident recovery process can generate idle wakeups; it is not
  a measurement of unrelated phone processes or total platform wakeups.
- Direct ALSA, `pw-play` and system-sound requests were rechecked after the final
  server tests: each produced RUNNING PCM, with active host output ports.
  Evidence: [audio-event-host.txt](results/audio-event-host.txt).
- Python compilation, shell syntax checks for helper/installer/test, sudoers
  validation and the installed user unit's `systemd-analyze --user verify`
  passed. The Java probe completed playback but retained runtime threads;
  tests bound its lifetime with `timeout 20` and require `final head=384000`
  even when cleanup returns timeout status 124.

Physical audibility cannot be confirmed remotely: these are playback, clock,
amplifier-power and fault-state checks, not a microphone recording or listener
confirmation. No reboot was performed because another agent is using this
phone and the task forbids restarting ztMETRO. Boot persistence is configured
but not reboot-tested. Full Pulse socket removal/recreation, which can leave a
stale container bind mount, is a separate unverified case; ordinary tested
service restarts retain the socket through systemd socket activation.

No changes were made to `ztmetro/ztmetro.c` or `ztos-android`; ztMETRO was not
restarted. Waydroid and ATL remain installed.

## Realtime

Tested on 2026-10-24, kernel 7.1.0-zboot, PipeWire 1.6.9. The phone and
rootfs overlay now carry persistent realtime scheduling configuration.

### Cause

The user manager and PipeWire unit had `LimitRTPRIO=0`, `LimitNICE=0`.
Consequently module-rt could not acquire FIFO directly and fell back to RTKit.
RTKit advertised maximum priority 20, so PipeWire clamped its requested 88 to
20. That clamp alone was not the failure: this boot's journal records successful
RTKit grants at priority 20 at 20:40:42 UTC on October 23.

Later restarts had no realtime scheduling. A diagnostic restart logged
`acquired realtime priority 20 ... using RTKit`, yet `ps -L` and `chrt -p`
showed SCHED_OTHER. A system-bus capture exposed the actual RTKit replies:
`org.freedesktop.DBus.Error.AccessDenied`, `Operation not permitted`, following
polkit authorization checks. Restarting RTKit did not repair this. The packaged
policy permits active/inactive local sessions but denies other subjects;
PipeWire runs beneath `user@1000.service`, rather than a seat session. This
session-dependent authorization is unsuitable for unattended audio services.
The success message is not sufficient evidence of scheduling policy. See
[filtered RTKit D-Bus evidence](results/audio-rt-dbus.txt).

### Persistent configuration

- `systemd/system/user@1000.service.d/50-audio-realtime.conf` gives the phone
  owner's user manager hard limits of RTPRIO 95, NICE -19 and MEMLOCK 64 MiB.
  A user-unit drop-in cannot raise its limits above the manager's hard limits.
- `systemd/user/{pipewire,pipewire-pulse}.service.d/50-audio-realtime.conf`
  requests those limits and a finite 200000 microsecond realtime CPU limit.
  PipeWire's existing module-rt acquires FIFO 88 directly; Pulse uses FIFO 83.
- `pipewire/{pipewire,pipewire-pulse}.conf.d/20-s9-realtime.conf` makes module-rt's
  soft/hard realtime CPU limits match the service limit. This prevents a failed
  attempt to raise the hard limit to infinity while retaining runaway protection.
- `polkit-1/rules.d/50-ztos-audio-realtime.rules` authorizes only RTKit's two
  priority acquisition actions for `zerone`, including outside a local seat
  session. RTKit retains its default priority ceiling, resource limits and
  watchdog. This also repairs authorization for clients using RTKit; it does
  not grant arbitrary polkit actions.

For live deployment the system and user managers were reloaded. Root `prlimit`
raised the already running user manager's hard limits to match the boot unit;
this avoided restarting the user manager or graphical session. A reboot will
apply the system drop-in normally. No PAM limits or RTKit daemon override is
needed. Temporary diagnostic RTKit/debug and baseline-test overrides were
removed. Existing 48 kHz / 512-frame ABOX settings remain in place.

`ztos-sounds` has one thread: its Pulse main loop both mixes and supplies samples.
It has no separate PipeWire client audio thread to promote. Its main event loop
remains SCHED_OTHER; the Pulse server's data loops servicing that stream run
FIFO 83. The sound helper and `ztmetro/` were not edited or restarted.

ABOX SPI 76 is GIC interrupt 108, Linux IRQ **106** in this boot. It uses
`request_irq()` with a hard interrupt handler, not a threaded handler; there is
no `irq/106-*` task to reprioritize. `irq/76-dwc3` is USB and must not be mistaken
for ABOX. No IRQ affinity, kernel driver, hardware clock or IRQ priority changed.

### Verification

`tests/audio-rt-load.py` runs four nice-0 busy loops pinned to little CPUs 0–3,
continuous 48 kHz stereo S16_LE two-tone music through `pw-play`, and system-sound
requests every 50 ms for 60 seconds. It captures line-buffered `pw-top` throughout,
checks playback/capture are still alive, records scheduling and cleans up its
children. Initial short captures and finite WAV trials were replaced by this
continuous-input probe so that music and monitoring cover the full interval.

The controlled baseline restored the original service limits through temporary
runtime drop-ins; the fixed run removed them and restarted PipeWire. Both used
`pw-play`'s default 4800-frame latency request. Each capture contains 61 active
speaker snapshots; useful values are within-run deltas, not counter comparisons
across restarts.

| 60-second mixed load | Before ERR | First fixed run ERR | Fixed repeat ERR |
| --- | --- | --- | --- |
| ABOX speaker, 512 frames | 55 → 78 (+23) | 0 → 0 | 0 → 1 (+1) |
| System sounds, 256 frames | 4 → 4 | 0 → 0 | 0 → 0 |
| Native `pw-play`, 4800 frames | 3 → 354 (+351) | 11 → 649 (+638) | 36 → 536 (+500) |

The baseline delivered 1188 sound requests; the first fixed run delivered 1180;
the fixed repeat delivered 1181. Playback and capture were alive at each end.
The first fixed run kept speaker and system-sound counters flat. The untouched
fixed repeat added one speaker error; system-sound errors stayed flat. **The
native music client's errors still increased under load in both runs.** Thus
the task's stronger all-ERR-flat acceptance condition is not met. These results
prove durable realtime scheduling and a substantial reduction in speaker errors,
not elimination of all playback crackle. The generated music uses continuous
raw stdin from a Python feeder; client errors may include producer/main-loop
scheduling, and do not alone identify a hardware underrun. No physical capture
was made to resolve that distinction.

One diagnostic RTKit call raised the first fixed run's `pw-play` main thread
to nice -11 during the interval; its errors continued increasing. The final
repeat made no priority changes during measurement. A supplemental 2048-frame
music request also retained native-client errors and added one speaker error;
no music-buffer override was deployed.

After installing the final configuration, two consecutive
`systemctl --user restart pipewire` operations produced new PIDs 264659 and
264857, each with `data-loop.0` at FIFO 88 and the main thread at nice -11.
The final 60-second mixed-load repeat ran after both restarts; the sound helper
reconnected and processed requests. The live manager and services retain
RTPRIO 95, NICE 39 (the raw rlimit value corresponding to -19), MEMLOCK 64 MiB,
and service RTTIME 200000 microseconds. Both RTKit acquisition actions passed
`pkcheck` for the service process. Installed user-unit verification and Python
syntax validation passed. No stress/capture/playback probe remained afterward.

Evidence: [baseline scheduling](results/audio-rt-before.txt),
[baseline graph](results/audio-rt-before-pw-top.txt),
[first fixed scheduling](results/audio-rt-after-initial.txt),
[first fixed graph](results/audio-rt-after-initial-pw-top.txt),
[final scheduling](results/audio-rt-after.txt),
[final graph](results/audio-rt-after-pw-top.txt),
[two restart checks](results/audio-rt-restarts.txt).

Rollback: remove the six realtime configuration files listed above (the system
manager drop-in, two user-unit drop-ins, two PipeWire snippets and polkit rule),
reload the managers, and restart PipeWire/Pulse. The live manager's raised hard
limits remain until its next restart or reboot; do not restart that manager
while another task depends on its graphical session. No executable or kernel
rollback is required.

No physical audio recording or reboot was performed. Boot persistence is
configured, not reboot-tested: another agent is using the phone, and this task
forbids restarting ztMETRO or Waydroid.
