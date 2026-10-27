# S9 microphone: continuous capture and controlled gain

Verified on Galaxy S9 / ztOS / PipeWire 1.6.9 on 2026-10-27.

## Cause and fix

The dropouts were in the WebRTC echo-cancel processing path, not the raw
microphone stream. A controlled replay of the old configuration produced 29
new processed-source ERR events and 29 silent gaps in ten seconds. Raw capture
had zero new errors during that same run. Gaps reached 1024 samples (21.33 ms).
The monitor-mode echo canceller also connected the speaker graph to capture:
`pw-top` showed the speaker following the microphone instead of its own driver.
The graph used power-of-two quanta while WebRTC required its own processing
blocks and a second monitor stream. The measurements localize the failure to
that two-stream processing path; they do not prove a particular internal
WebRTC queue implementation defect.

Replace that module with one capture-only filter-chain:

- 100 Hz high-pass, Q 0.707.
- LSP mono limiter with input gain 8 (+18.06 dB), 5 ms lookahead,
  threshold 0.891 (-1 dBFS), gain boost disabled, automatic level regulation
  enabled, no oversampling or dithering.
- Preserve `s9-microphone-agc` and its priority so saved default-source choices
  and Waydroid's PulseAudio client keep working. The name is retained for
  compatibility; this is fixed gain with peak limiting, not speech AGC.
- Capture targets `s9-microphone`, is passive, and permits reconnection.
  The existing 1024-frame ALSA period and 1024/48000 filter latency work without
  changing global latency, speaker configuration, or the kernel driver.

The graph follows the [PipeWire filter-chain source configuration](https://docs.pipewire.org/page_module_filter_chain.html).

The installed configuration is
`devices/samsung-starlte/overlay/etc/pipewire/pipewire.conf.d/61-s9-microphone-agc.conf`.
The LSP LADSPA library was already installed for the speaker limiter. Codec
`mic_gain=48` (+24 dB) remains unchanged. No kernel rebuild is necessary.

After a clean `systemctl reboot`, the default source was automatically
`s9-microphone-agc`, Waydroid was running, and capture and playback had separate
1024- and 512-frame driver graphs. No user service restart or manual recovery
was required. The final active filter spent about 136 microseconds per 21.33 ms
cycle in one sampled row (about 0.64% of one core); this is a processing-cost
sample, not a full UI benchmark.

## Measurements

The host probe records through PulseAudio (`parec`) at 48 kHz, mono S16LE. It
captures for 11 seconds and retains the final ten seconds to exclude startup.
It records `pw-top` concurrently and reports active-row ERR deltas, RMS/peak,
zero runs of at least 1 ms, and adjacent jumps greater than half full scale.
Initial `C` rows in `pw-top` contain placeholder zeros and are excluded.

| Test, ten seconds each | RMS dBFS | Peak dBFS | Silent gaps >=1 ms | New source ERR |
| --- | ---: | ---: | ---: | ---: |
| Original default, ambient, before edits | -48.50 | -28.19 | 30 | Not retained |
| Old module replay, ambient, after boot | -44.47 | -19.88 | 29 | 29 |
| New default, speaker reference tone | -12.33 | -8.07 | 0 | 0 |
| Android AudioRecord, speaker reference tone | -12.39 | -9.01 | 0 | 0 on concurrent host graph |
| Android AudioTrack playback, captured through new default | -13.93 | -5.92 | 0 | 0 |

Reference: 1 kHz sine, amplitude 0.03 full scale, played through the phone's
default speaker at 100% sink volume; raw microphone volume was also 100%.
AEC is absent in the replacement, so the tone is not cancelled. Android playback
used an equivalent amplitude of 983/32768. Android AudioTrack reported 480,000
played frames; AudioRecord returned 960,000 bytes (ten seconds, mono S16LE).
The Android probe uses root `app_process`, so it verifies the Android HAL path,
not a particular recorder application's permissions or encoding.

The final tone run had zero new ERR on raw capture, both filter nodes, speaker,
speaker filter, and PulseAudio recorder, and zero half-scale jumps. Existing
startup counters were raw=2, filter capture=2, filter output=1; they remained
unchanged. A separate ambient run reached exactly -1.003 dBFS, confirming the
limiter ceiling. That ambient run contained 13 large sample jumps, so large
jumps alone must not be treated as proof of dropped audio. No silent gaps were
present. An initial Android capture had 1.83 ms of startup zeros only; the
subsequent reference-tone capture had none.

Ambient before/after levels are not a calibrated speech comparison. No person
spoke a controlled phrase during testing. The reference is in the requested
-18..-12 dBFS range, but speech depends on distance and voice level. Fixed gain
cannot guarantee that range for every speaker. Removing WebRTC also removes
its noise suppression and acoustic echo cancellation; recordings include room
noise and speaker playback. Conferencing applications may supply their own AEC.

## Reproduce and tune

Copy `audio/tools/mic-measure.py` to the phone, then run as its desktop user:

```sh
XDG_RUNTIME_DIR=/run/user/1000 python3 /tmp/mic-measure.py /tmp/mic-check --tone
XDG_RUNTIME_DIR=/run/user/1000 python3 /tmp/mic-measure.py /tmp/mic-ambient
XDG_RUNTIME_DIR=/run/user/1000 python3 /tmp/mic-measure.py /tmp/mic-raw --source=s9-microphone --tone
```

Each invocation writes WAV, raw PCM, JSON and `pw-top` text under its prefix.
Without `--source`, it uses the same default PulseAudio source as Waydroid.
Zero-run detection is useful for this failure but cannot detect every possible
nonzero discontinuity. Compare ERR deltas and the waveform as well.

For the Android check, compile `audio/tools/MicProbe.java` against an Android
SDK `android.jar` with `javac --release 8`, then convert its class with D8.
Copy the resulting `classes.dex` to Android `/data/local/tmp/mic-probe.dex`.
On this phone that directory is backed by
`/home/zerone/.local/share/waydroid/data/local/tmp` on the host. Run:

```sh
sudo waydroid shell -- sh -c 'CLASSPATH=/data/local/tmp/mic-probe.dex app_process / MicProbe play'
sudo waydroid shell -- sh -c 'CLASSPATH=/data/local/tmp/mic-probe.dex app_process / MicProbe record /data/local/tmp/mic-probe.raw'
```

Run the host tone probe concurrently with Android recording. Its raw file is
48 kHz mono signed 16-bit little endian. Use an external timeout for manual
probes so an audio-service failure cannot leave the test waiting indefinitely.

To adjust loudness, change `Input gain (G)` in the overlay: 4 gives +12.04 dB,
8 gives +18.06 dB, 12 gives +21.58 dB. Keep the -1 dBFS threshold and gain boost
disabled. Measure a normal spoken phrase before increasing gain further; extra
gain also amplifies room noise. If raw PCM clips, reduce codec `mic_gain` first;
a downstream limiter cannot repair ADC/codec clipping. Codec units are 0.5 dB,
and runtime changes apply on the next capture open.

Install the same overlay file into `/etc/pipewire/pipewire.conf.d/`, then reboot
to apply it and check both Android recording and playback. Avoid restarting
PipeWire under a running Waydroid session. To roll back this change, restore the
previous version of that one file and reboot; no user data or driver changes
are involved. Rollback also restores the measured WebRTC dropout behavior.
