# Audio on the Galaxy S9 (Exynos 9810) under mainline

The hardware, from the stock device tree: every sound passes through
**ABOX**, an audio DSP block in the SoC (a Cortex-A7 running Samsung's
"calliope" firmware), on its way to two **MAX98512** amplifiers (I2C: U8003 the speaker, U8002 the
earpiece) or a **CS47L93** codec (U8004, SPI; headphones and microphones).
Mainline has a driver for the codec only.

## How this is being developed

The running kernel loads modules and has `/dev/mem`, so nothing here needs a
new boot image: build a module on the laptop against `build/linux-7.1/out`,
copy it over Wi-Fi, `insmod`. A fault restarts the phone (`panic=5`) into
the same system with the module gone.

    make -C build/linux-7.1 O=out M=$PWD/audio/probe KBUILD_MODPOST_WARN=1 modules

`audio/probe/peek.py BASE OFFSET...` reads registers through `/dev/mem`.
Samsung's driver is in `third_party/vendor-kernel/sound/soc/samsung/abox/`
(GPL, a sparse checkout of LineageOS's exynos9810 kernel), the firmware in
`stock/firmware/` (copied from the phone's own VENDOR partition; on the phone
in `/lib/firmware/`).

## Where it stands

**Playback through the two speakers works as an ALSA card**, loaded as
modules on the kernel that is already flashed. PipeWire picks it up as the
default sink by itself.

- `audio/driver/snd-zrn-abox.c`: the driver. It starts the DSP firmware
  (audio IOMMU set up by hand, firmware into SRAM and where the DSP expects
  its DRAM, the DSP's own interrupt controller, the core released; the
  firmware reports `ABOX_BOOT_DONE`, version `CRL0`, about 1.3 s later), takes
  the DSP's interrupt (SPI 76), and offers one playback device: 48 kHz, 16
  bit, stereo, whose buffer is the DSP's RDMA0 buffer. The stream is routed
  SPUS -> SIFS0 -> UAIF1 (I2S; bit clock from the audio PLL by two dividers;
  pins gpb1-0..3). The two MAX98512 amplifiers (0x39 left, 0x3a right) sit on
  a bit-banged I2C bus the driver registers on gpp4-5 (data) and gpp4-4
  (clock), and are set up when a stream is prepared and shut down when it is
  closed. One mixer control, `Speaker Playback Volume`.
- `audio/build.sh` builds it and the sound core (soundcore, snd, snd-timer,
  snd-pcm, i2c-gpio) into `audio/modules/`; on the phone they live in
  `/lib/modules/7.1.0-zboot/extra`.
- `zrn-audio.service` loads the driver at boot through
  `/usr/local/sbin/zrn-audio`, which leaves a mark while it does: if loading
  ever stops the kernel, the next boot skips it (remove
  `/var/lib/zrn-audio/loading` to retry).
- `audio/probe/` has the steps that led here: `zrn_abox.c` (DSP boot, then a
  test tone without ALSA), `zrn_amp_i2c.c` and `audio/amp.sh` (the amplifiers
  by hand), `peek.py`.

How it was checked without ears: with a stream running the amplifiers report
powered up and stay up with their clock monitor on; with the stream stopped
and the monitor on they shut themselves down for lack of a clock. The DSP's
read position advances through the buffer and period interrupts arrive.

## Three things that were wrong, for whoever touches this next

- **The firmware must be told its core clock.** It asks for a "gear" and waits
  for `ABOX_CHANGED_GEAR` with the frequency in Hz; untold, it assumes PLL/12
  while the core runs at PLL/4, and plays three times too fast.
- **Period interrupts are interrupt numbers, not messages.** Interrupt 11 on
  the DSP's controller is "stream 0 played a period"; nothing is put in the
  mailbox for it.
- **The position register counts in units of 16 bytes in both fields and
  wraps at 64K**, whatever Samsung's `abox_rdma_pointer()` suggests. The ALSA
  buffer is capped at 64K for that reason.

- **The amplifiers' boost converter must be taken out of bypass** (0x86..0x88).
  In bypass, on this chip revision, the output stage has no supply: the
  chip reports powered up, takes its clocks, and is silent. Its measurement
  ADC (0x41 = 7, then read 0x4b) shows the supply and is the quickest check.
  **The measurement ADC must be running too** (0x41..0x49 as Samsung sets
  them): with bypass off and the supply reading right, there was still no
  sound until it was enabled.
  Envelope tracking (0x84) must stay off: enabled as in Samsung's driver, it
  pulled the supply back down to its minimum, which is inaudible.

Also: the amplifiers' brown-out limiter, enabled without its per-level
limits, silences them; and changing the audio bus or DSP core clock dividers
while the DSP runs hangs the phone (`audio/probe/rate.sh` measures the
playback rate against 48 kHz; 1.00 is right).

## Music visualizer capture

The Music app's original Cava capture kept the speaker sink active even
while playback was paused and requested 512-frame processing blocks. This
reproduced severe PipeWire underruns and distorted playback; closing the
app did not reliably recover the already affected audio state.

`apps/music/engine.py` now sets Cava's PipeWire input to `active = 0` and
48 kHz, and gives only the Cava process `PIPEWIRE_LATENCY=2048/48000` to
override its default latency request. The visualizer remains enabled but
does not keep idle speakers running. The audio driver was restored unchanged
after unsuccessful diagnostic experiments.

After a TWRP hot reboot, two consecutive playback/stop cycles through the
updated app ran with Cava and mpv active at 48 kHz, 2048-frame graph blocks,
and zero reported errors. ALSA closed after each stop. Evidence:
`build/power-analysis/music-passive-final.log` (2026-10-16).

## Keyboard-sound regression (2026-10-16)

The keyboard helper's low-latency streaming implementation repeatedly
uncorked/corked an underfilled stream. Together with a 1024-frame graph default,
this reproduced about 47 speaker-sink underruns per second during playback.
The helper now predecodes the supplied WAVs and mixes a maximum of four voices
into one reusable event stream. Full blocks are always supplied, including
silent tails. Fast taps replace the oldest voice instead of creating more
streams or queuing old taps. A 500 ms idle grace lets samples finish before
corking. Keyboard volume scaling is retained and the helper is excluded from
media detection. It reconnects when PipeWire restarts.

The phone overlay fixes the graph at 48 kHz / 2048 frames. A device-specific
WirePlumber rule sets the ABOX period and headroom to 2048 frames and uses
hardware interrupts (`api.alsa.disable-tsched = true`). Buffer increases alone
still produced errors; the interrupt setting removed them in the mixed test.
Other audio devices do not receive this rule. `rtkit` is included for PipeWire's
real-time scheduling support.

Validation on the phone: playback, two keyframe seeks, pause/resume, the Music
app's passive Cava visualizer (236 frames), and 90 mixed keyboard taps ran with
zero reported speaker errors; ALSA closed after playback stopped. Evidence:
`build/power-analysis/keyboard-audio-baseline.log` and
`build/power-analysis/keyboard-audio-fixed.log`.

The four-voice pool also passed a 600-request burst at 100 requests/second over
playback: exactly one keyboard stream, no increase in the speaker error counter
(8 before/throughout), and ALSA closed after idle. Evidence:
`build/power-analysis/keyboard-pool-burst.log`. The tap stream was subsequently tuned from an 85 ms negotiated buffer to
32 ms (`minreq=512` frames, while the speaker graph stays at 2048). A repeated
600-request burst again left the speaker error counter unchanged. Single taps
now use the requested volume; only overlapping voices share headroom.
Evidence: `build/power-analysis/keyboard-pool-low-latency.log`.
Mixer checks with address and
undefined-behavior sanitizers cover oldest-voice replacement, the four-voice
bound, clipping, and silent tails.

The changes are installed on the test phone and included in fresh phone images.
The ABOX kernel module and hardware clock registers were not changed.

## Responsive keyboard taps

The S9 IRQ path also passes at 512-frame hardware periods. The phone now uses
512-frame periods, eight periods of ring capacity, and 256 frames of headroom;
the graph minimum is 512 while the default/maximum remain 2048 for other
outputs. On ABOX the observed IRQ-driven graph stays at 512 frames (10.7 ms),
including with clients requesting larger blocks.

The tap stream requests a 512-frame target and 128-frame minimum write; the
Pulse server negotiates 256-frame writes and a 768-frame (16 ms) tap buffer.
A flush before uncorking removes silence queued before idle. Full writes,
four bounded voices, overlap balancing, and the idle cork are retained.

Validation on the phone: 600 tap requests at 100/second over playback, then
separated tap bursts over mpv/Cava with seeking and pause/resume. Speaker and
tap stream error counts stayed zero, only one keyboard stream existed, and
ALSA closed afterward. The original WAVs have less than 0.1 ms of leading
silence above 5% of peak. These are negotiated buffering and graph timings,
not a physical speaker latency measurement. Evidence:
`build/power-analysis/typing-responsive-report.json`,
`build/power-analysis/typing-responsive-top.log`, and
`build/power-analysis/typing-transitions-top.log`.

## Not done

- **Speaker protection.** On the stock system the DSP runs Maxim's DSM
  against the current and voltage the amplifiers report. Not here: so the
  volume control stops at -4 dB, the analogue gain is +9 dB (stock +15) and
  the boost output about half way to stock's. It will be quieter than stock.
  Do not raise those without protection.
- **Earpiece**: the second MAX98512 (U8002), on the same amplifier I2S and
  I2C as the speaker's, not on the codec (service manual, troubleshooting p3
  and p29).
- **Headphones, microphones**: on the CS47L93 codec (SPI), whose bus is not in
  the device tree either. Both microphones are digital and take their 1.8 V
  from the codec (MICBIAS), so they stay silent until the codec drives it. Mainline has the codec driver
  (madera), so this is the next piece.
- **Idle power**: the DSP core stays running after the first load.
- Other rates and formats than 48 kHz / 16 bit / stereo (PipeWire converts).
