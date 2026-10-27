# Microphones on the Galaxy S9: the stock capture path, and capture in snd-zrn-abox

Status 2026-10-27: researched offline and written, **not run on the phone**. The
code is in `audio/driver/snd-zrn-abox.c` behind `capture=1` (default off). It
compiles against `build/linux-7.1/out-snd` (laptop tree; build the real module on
the workstation, see "Test").

Everything below comes from files in this repo. No phone access, no REA run on
binaries was needed: the stock mixer paths, the stock DT and Samsung's GPL
sources answer every question except the ones listed under "Unknowns".

## The stock path, in one line

DMIC1 + DMIC3 (digital mics, powered from MICBIAS1A/1C) -> CS47L93 IN3L/IN3R ->
AIF1TX1/2 -> codec AIF1 pins (I2S, codec is slave) -> ABOX **UAIF0** (SoC is master,
1.536 MHz bit clock) -> **NSRC0** -> **SIFM0** = WDMA -> **WDMA1** (pcm device 9) ->
memory, 48 kHz, 16 bit, stereo.

## 1. Evidence: the stock capture path

### Board wiring (stock/audio-etc/mixer_paths.xml)

- Lines 5-8: `UAIF0 - CODEC for normal Playback and Capture`, UAIF1 the speaker
  amps, UAIF2 the codec again for voice recognition (VTS), UAIF3 Bluetooth.
- Line 30: `<pcmdai capture_link="9" />`: the HAL records from pcm device 9.
  Devices 0-7 are RDMA0-7 and 8+ are WDMA0+, so 9 is **WDMA1** (DSM.md already
  shows that SIFM3 feeds WDMA4, so SIFMn feeds WDMA(n+1)).
- Lines 52-55: `ABOX UAIF0 width 16, channel 2, rate 48000`.
- Line 91: `ABOX SPUM ASRC1 On` (NSRC0's rate converter); line 98:
  `ABOX SIFM0 = WDMA`.
- Lines 120-130, codec defaults: `IN3 OSR 3.072MHz`, `IN3L HPF Switch 1`,
  `IN HPF Cutoff Frequency 10Hz`, `Input Ramp Up 8ms/6dB`.
- Line 225 `route-uaif0-to-nsrc0`: `ABOX NSRC0 = UAIF0`. Line 253
  `route-nsrc0-to-wdma1`: `ABOX SIFM0 = WDMA`.
- Line 322 `route-main-mic` (the Universal9810 board section):
  `AIF1TX1 Input 1 = IN3L`, `AIF1TX2 Input 1 = IN3R`, `DMIC1 Switch 1`,
  `DMIC3 Switch 1`.
- Line 389 `dev-main-mic` (the Star section, what the S9 HAL actually uses):
  `LHPF1 Input 1 = IN3L`, `ASRC1IN1L Input 1 = LHPF1`, `AIF1TX1 = AIF1TX2 =
  ASRC1IN1L`, `DMIC1 Switch 1`: one mic, high-pass filtered, through the codec's
  ASRC (because stock runs the inputs and AIF1 in different clock domains),
  copied to both channels.
- Line 1760 `media-mic` = `route-ap-record` (UAIF0 -> NSRC0 -> WDMA1) +
  `dev-main-mic`. `recording-mic` (line 1789) and `recognition-mic` (line 1865)
  both resolve to `media-mic`. There is no path literally named `handset-mic`
  or `main-mic`; those names in the request map to `media-mic` /
  `media-main-mic` (line 1723: `route-uaif0-to-nsrc0`, `route-nsrc0-to-wdma1`,
  `route-main-mic`).
- `dev-2nd-mic` (line 397) is DMIC4 on IN4L; `dev-dual-mic` puts main on left and
  2nd on right.

### The codec (stock/twrp-live.dts)

- Line 7393 `spi_2: spi@10470000` (samsung,exynos-spi), pins `spi2_bus` (line 1947:
  gpp0-4/5/6, function 2) and `spi2_cs_func` (gpp0-7).
- Line 7410 `cs47l93@0`: chip select 0, `spi-max-frequency` 25 MHz, interrupt on
  gpa2-2 (`codec_irq`, line 431), `reset-gpios` gpg1-0 (`codec_reset`, line 2287).
- Supplies: AVDD/DBVDD1/CPVDD1 = LDO33 `vdd_pmic_codec_1p8` (line 8999,
  **always-on, boot-on**); CPVDD2/DCVDD = LDO34 `vdd_codec_1p2` (line 9010,
  **not** always-on, 5 ms enable ramp). Both on the S2MPS18 PMIC (line 8635).
- Line 7427 `cirrus,dmic-ref = <0 1 1 1>`: IN3's DMIC is supplied from MICBIAS1.
  `MICBIAS1` 1.8 V with external cap, `micvdd` 3.0 V (the CP2 regulator node).
- `sound` node (line 12615, `samsung,star-madera`): `cirrus,sysclk = <1 4
  98304000>` (FLL1), `cirrus,asyncclk = <2 5 98304000>` (FLL2),
  `cirrus,fll1-refclk = <1 0 26000000>` (MCLK1, 26 MHz from the SoC's
  `xclkout`), `cirrus,fll2-refclk = <2 8 26000000>` (source 8 = AIF1BCLK; the
  rate is overwritten at runtime). Routing: DMIC1 <- MICBIAS1A, IN3L <- DMIC1;
  DMIC3 <- MICBIAS1C, IN3R <- DMIC3; DMIC4 <- MICBIAS1B, IN4L <- DMIC4.
- UAIF0's pins: `aud_i2s0_bus` (line 569): gpb0-1..4, function 2. gpb0-0 is
  `aud_codec_mclk`.

### Machine driver (third_party/vendor-kernel/sound/soc/samsung/star_madera.c)

- Line 1296: UAIF0 link `SND_SOC_DAIFMT_I2S | NB_NF | CBS_CFS`: the codec is bit
  and frame clock slave, ABOX drives both.
- `star_uaif0_madera_hw_params` (line 278): `fll2_refclk.rate = rate * width * 2`
  (1.536 MHz), ASYNCCLK 98.304 MHz. `star_uaif0_madera_prepare` (line 303):
  `snd_soc_dai_set_tristate(cpu_dai, 0)` (bit clock on) **before** starting FLL2
  from AIF1BCLK. So stock already locks a codec FLL to ABOX's bit clock.
- `star_uaif0_init` (line 894): SYSCLK from FLL1, AIF1 on ASYNCCLK.

### ABOX (third_party/vendor-kernel/sound/soc/samsung/abox/)

- `abox.c:2232` `nsrcx_texts`: NSRC value 8 = UAIF0; field `ROUTE_CTRL1[3:0]`
  for NSRC0 (`abox.h` ABOX_ROUTE_NSRC_L(0)).
- `abox.c:2285` `sifmx_texts`: "WDMA" = 0, "SIFMS" = 1, field
  `SPUM_CTRL0` bit `ABOX_FUNC_CHAIN_NSRC_OUT_L(0)` = 4 + 3 = **bit 7**; NSRC0's
  ASRC is bit 4 (`abox.h:148`, base 4, interval 4).
- `abox_if.c:252` `abox_uaif_set_fmt`: I2S -> WS_MODE 0; NB_NF -> BCLK_POLARITY 1,
  WS_POLAR 0; CBS_CFS -> `CTRL0.MODE` (bit 2) = 1.
- `abox_if.c:320` `abox_uaif_set_tristate`: `ABOX_QCHANNEL_DISABLE` (0x38) bit
  `ABOX_BCLK_UAIF0` (= 2, `abox.h:470` enum qchannel): forces the bit clock on.
- `abox_if.c:350` `abox_uaif_hw_params`: CTRL1 SBIT_MAX = width-1, SLOT_MAX =
  channels-1, FORMAT; for 16/2 this is the same word snd-zrn-abox already writes
  to UAIF1 (`(9 << 24) | BIT(23) | (1 << 18) | (15 << 12)`).
- `abox_if.c:423` `abox_uaif_trigger`: capture sets `CTRL0.MIC_ENABLE` (bit 1).
- `abox_wdma.c:117` `abox_wdma_hw_params`: IPC_PCMCAPTURE, `task_id =
  channel_id = id`, `PCM_SET_BUFFER` (IOVA_WDMA_BUFFER(id), period bytes,
  periods), `PCM_PLTDAI_HW_PARAMS` (rate, width, channels). `abox_wdma_prepare`
  (236): `PCM_PLTDAI_PREPARE`. `abox_wdma_trigger` (271): `PCM_PLTDAI_TRIGGER` 1/0.
  Open/close: `PCM_PLTDAI_OPEN/CLOSE`. Same messages as the RDMA side, with
  IPC id 3 instead of 2.
- `abox_wdma.c:327` `abox_wdma_pointer`: the WDMA status register (`abox.h:309`
  base 0x2000, interval 0x100, STATUS +0x30 on 9810) with the same
  PROGRESS / RBUF_OFFSET / RBUF_CNT fields as RDMA.
- `abox_wdma.c:66` `abox_wdma_irq_handler`: period interrupts come as
  `IPC_PCMCAPTURE` / `PCM_PLTDAI_POINTER` with the channel id; `abox.c:4494`
  also has a GIC line `WDMA0_BUF_FULL` = 8, `WDMA1_BUF_FULL` = 9
  (`include/sound/samsung/abox_ipc.h:331`).
- `abox.h:361` `IOVA_WDMA_BUFFER(x) = 0x92000000 + 1 MiB * x`.
- `abox.c:1211` `abox_sample_rate_put_ipc` / `abox.c:758`
  `abox_sif_format_put_ipc`: IPC_ABOX_CONFIG `SET_INMUX0_SAMPLE_RATE` (5,
  param1 = rate) and `SET_INMUX0_FORMAT` (0x14, param1 = width, param2 =
  channels): what `abox_hw_params_fixup_helper` sends for SIFM0 capture.
- CMU (`drivers/soc/samsung/cal-if/exynos9810/cmucal-sfr.c:139-146`):
  `DIV_CLK_AUD_UAIF0` at 0x1824, `MUX_CLK_AUD_UAIF0` at 0x1004 (UAIF1's are
  0x1828 / 0x1008, which the driver already sets).

### Codec registers (mainline, build/linux-7.1)

- `drivers/mfd/madera-core.c:33` `CS47L92_SILICON_ID 0x6371` (CS47L93 shares it);
  `madera_wait_for_boot` (240): poll `IRQ1_RAW_STATUS_1` (0x1880) bit 7, then ack
  in `IRQ1_STATUS_1`; reset low >= 2 ms.
- `drivers/mfd/cs47l92-tables.c:20` `cs47l92_reva_16_patch` (applied after boot);
  `:1881` SPI regmap: 32-bit register, 16 bits pad, 16-bit value, big endian
  (read flag 0x80 in the first byte, regmap-spi default). Register defaults,
  e.g. GPIO5..8 CTRL_1 = 0x2001 (GPIO function), so AIF1's pins must be
  switched to function 0 (`pinctrl-cs47l92.c`: aif1 = pins 4..7).
- `sound/soc/codecs/madera.c:4407` `madera_fllhj_apply` /
  `:4553 madera_fllhj_enable`: the FLL sequence copied by `codec_fll_on`.
- `madera.c:1092` `madera_configure_input_mode`: on CS47L92 only IN1/IN2 are
  analogue; IN3 is digital-only, needing just DMIC_SUP in `IN3L_CONTROL`.
- `madera.c:2253` `madera_in_ev`: unmute, then `IN_VU`.
- `drivers/regulator/arizona-micsupp.c:203` MICVDD = `LDO2_CONTROL_1` VSEL +
  `MIC_CHARGE_PUMP_1` CPMIC_ENA/BYPASS; 3.0 V = selector 0x24.

## 2. Design

**One capture device, hw:0,2 "Microphone"**, 48 kHz, S16_LE, stereo (left =
main mic DMIC1/IN3L, right = DMIC3/IN3R; the S9 has the two bottom mics there).
Mono is left to PipeWire.

The one deliberate change from stock: **the codec runs entirely from ABOX's bit
clock.** FLL1 is referenced to AIF1BCLK (1.536 MHz) and makes SYSCLK 98.304
MHz; AIF1 and the inputs both run on SYSCLK rate 1. That is the
Universal9810 `route-main-mic` (IN3L/IN3R straight to AIF1TX1/2) and needs no
codec ASRC, no LHPF, no ASYNCCLK, and no 26 MHz MCLK1 (whose source, the SoC's
`xclkout`, nothing on mainline turns on). Stock already locks FLL2 to the same
bit clock, so the clock source itself is proven.

What the driver programs, in order:

1. **Load (capture=1):** a 64 KiB coherent buffer mapped in the ABOX IOMMU at
   `IOVA_WDMA(1)` = 0x92100000, and the PCM device. No hardware is touched.
2. **Open:** codec power-up once: LDO34 on through `s9p-acpm` if it is loaded
   (`symbol_get`: no link dependency, playback never needs that module),
   reset pulse on gpg1-0, poll BOOT_DONE, check ID 0x6371 (**if the ID is wrong,
   nothing more is written and the codec goes back into reset**), ack, apply the
   rev A patch, AIF1 pins to their audio function. Then `SET_INMUX0_SAMPLE_RATE`
   48000 and `SET_INMUX0_FORMAT` 16/2, and `PCM_PLTDAI_OPEN` on WDMA1.
3. **hw_params:** `PCM_SET_BUFFER` (0x92100000, period bytes, periods) and
   `PCM_PLTDAI_HW_PARAMS` (48000, 16, 2), as for RDMA.
4. **Prepare (sleepable):** ABOX side: UAIF0 divider 47 and mux, gpb0-1..4 to I2S,
   UAIF0 CTRL1 as UAIF1, CTRL0 MODE master, NSRC0 <- UAIF0, SIFM0 -> WDMA, NSRC0
   ASRC on, `QCHANNEL_DISABLE` bit 2 (bit clock running). Then the codec: FLL1
   from AIF1BCLK, wait for lock (30 ms max; fail = prepare fails and the path is
   undone), SYSCLK 98.304 MHz, MICVDD 3.0 V, MICBIAS1 1.8 V with 1A and 1C, IN3
   DMIC (3.072 MHz OSR, HPF, supply MICBIAS1), AIF1 slave I2S 16/16, 32 BCLK per
   frame, TX1 <- IN3L, TX2 <- IN3R, TX enables, inputs on, unmute at 0 dB, VU.
   Then `PCM_PLTDAI_PREPARE`.
5. **Trigger:** `UAIF0_CTRL0.MIC_ENABLE` and `PCM_PLTDAI_TRIGGER` 1; stop is
   the reverse.
6. **Pointer:** WDMA1 status at 0x17c52130, decoded exactly as playback's
   RDMA pointer (same fields, only ever forward).
7. **Periods:** GIC line 9 in `abox_isr`, and `IPC_PCMCAPTURE` /
   `PCM_PLTDAI_POINTER` for channel 1 in `rx_one`; both call
   `snd_pcm_period_elapsed`. WDMA2 (voice TX) and WDMA4 (DSM V/I) messages are
   ignored as before.
8. **Close:** `PCM_PLTDAI_CLOSE`, codec down (mute, inputs/AIF/MICBIAS/MICVDD
   off, SYSCLK off, FLL1 off), then `MIC_ENABLE` off, q-channel back, NSRC0
   back to RESERVED.

Robustness hooks, all only when the microphone is open:

- System sleep is **refused** (`-EBUSY`) while hw:0,2 is open, like a call or
  DSM. The codec and UAIF0 have no suspend/resume path yet.
- `dsp_recover` stops the capture stream (XRUN), and before a power cycle sends
  WDMA1 trigger 0 + close and waits for WDMA1 to stop as well: power taken from
  under a running DMA froze the phone once (2026-10-26).
- After a DSP restart the next prepare rebuilds routes, INMUX config and the DMA.

## 3. The change (audio/driver/snd-zrn-abox.c)

- `capture` (bool, 0444, default 0) and `cap_wdma` (int, default 1) parameters,
  state, counters `cap_irqs`, `cap_msgs`, `codec_errors` (line ~304).
- `rx_one`: one new `else if` for `IPC_PCMCAPTURE` pointer messages (line ~786).
- `abox_isr`: GIC line 8 + cap_wdma marks a capture period (line ~823).
- New block after `abox_pcm_ops` (lines ~1745-2420): SPI bit-bang
  (`codec_xfer`), `codec_supplies`, `codec_power_up`, `codec_fll_on/off`,
  `codec_start/stop`, `cap_path_on/off`, `cap_config`, the `cap_*` PCM ops, and
  the `codec_peek` debug parameter.
- `teardown`: path off, codec into reset, GPIOs released, buffer freed.
- `abox_pm_event`: refuse sleep while capture is open.
- `dsp_recover`: capture stopped and its DMA waited for (guarded by
  `capture && cap.opened`).
- `abox_init`: buffer + IOMMU mapping + the PCM, all under `if (capture)`.

**With capture=0 nothing in the playback path changes**: every new line is
behind `capture` (the only always-run code is a `bool cap_period = false` and
two `if (capture ...)` tests in the interrupt handler).

Build check (laptop tree, compile only):

    PATH=$PWD/build/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin:$PATH \
    make -C build/linux-7.1 O=out-snd M=$PWD/audio/driver ARCH=arm64 \
      CROSS_COMPILE=aarch64-none-linux-gnu- KBUILD_MODPOST_WARN=1 modules

No compiler warnings. `KBUILD_MODPOST_WARN=1` is required, as in audio/build.sh,
because the snd-* symbols live in modules out-snd does not list. `s9p_acpm_*`
is not an import of the module (fetched with `symbol_get`).

## 4. Test procedure

**Build on the workstation** (itelasoft@192.168.195.38) with `audio/build.sh`:
the phone's running kernel came from that tree, and a module built against the
laptop tree has oopsed before (memory: kernel/module tree mismatch).

Read before starting:

- **Reboot, never rmmod.** An oops in an rmmod once left the phone unable to
  shut down. Each change of parameter = edit modprobe.d + reboot.
- `panic=5` and `ztoscirrusaudio`'s loading mark mean a hang at load reboots
  the phone and the next boot still tries (the script clears a stale mark);
  if it loops, see "Back out".
- Keep the phone on USB (172.16.42.1) so the console is reachable.

Steps:

1. Install the new module, still with capture off, and reboot:
   `install -m644 snd-zrn-abox.ko /usr/lib/modules/$(uname -r)/extra/ && depmod -a`
   then `systemctl reboot`.
   Check playback is unchanged: `paplay` something; `cat
   /sys/module/snd_zrn_abox/parameters/capture` = `N`; `arecord -l` shows no
   card-0 capture device. **Stop here if playback changed at all.**
2. Check the codec's supplies (needs zrn-bbd loaded):
   `grep -E "^LDO3[34] " /sys/class/sec/sensorhub/ldo`.
   LDO33 should be mode 3. LDO34 may be 0; the driver switches it on itself at
   first open if s9p-acpm is loaded. Write it down either way.
3. Turn capture on and reboot:
   `echo "options snd-zrn-abox capture=1" > /etc/modprobe.d/zrn-capture.conf`,
   `systemctl reboot`.
   After boot: `cat /sys/module/snd_zrn_abox/parameters/capture` = `Y`,
   `arecord -l` lists `card 0 ... device 2: Microphone`. Playback still works.
4. First open, ID only, with a log watch running:
   `dmesg -w | grep snd-zrn-abox &`
   `arecord -D hw:0,2 -f S16_LE -r 48000 -c 2 -d 1 /tmp/m0.wav`
   Look for, in order:
   - `codec supplies: LDO33 .., LDO34 ..` (or the s9p-acpm warning)
   - `codec id 6371 rev ....`: **if the id is ffff or 0000, stop.** The codec
     is unpowered, still in reset, or the SPI pin roles are wrong (MOSI/MISO
     swapped). Nothing else was written. Report the line.
   - `codec FLL1 did not lock`: the bit clock is not reaching the codec (UAIF0
     pins or q-channel). Report `cat /sys/module/snd_zrn_abox/parameters/codec_errors`.
   - `codec capture on (inputs 0030, AIF1 TX 0003, sysclk 0444)`: the codec side
     is up.
5. A real recording: speak or tap near the bottom of the phone.
   `arecord -D hw:0,2 -f S16_LE -r 48000 -c 2 -d 5 /tmp/m1.wav`
   then `sox /tmp/m1.wav -n stat` (or copy it to the laptop and listen).
   Check:
   - the file is 5 s long (`soxi`): the DMA is moving at 48 kHz;
   - `cat /sys/module/snd_zrn_abox/parameters/cap_irqs cap_msgs`: at least one
     of them grows with each period (which one tells us how the DSP reports);
   - RMS clearly above zero and changing with sound. Silence (all zero) with a
     5 s file means data is not reaching UAIF0: read codec registers with
     `echo 1881 > .../codec_peek; cat .../codec_peek` (FLL lock bit 0x0100),
     `echo 0301 > ...` (input status).
6. Playback and capture together: play through PipeWire while recording; both
   must keep going. Then `arecord` repeatedly (10 x 2 s) to catch start/stop
   problems.
7. Sleep: with nothing open, `zrn-sleepd` should sleep as before. With
   `arecord` running, suspend must be refused (that is intended for now).
8. Only after all that: PipeWire picks the device up as a source on its own
   (ALSA card 0, device 2); test with `pw-record`.

Back out:

- `rm /etc/modprobe.d/zrn-capture.conf && systemctl reboot` restores today's
  behaviour with the new module.
- If loading itself hangs or loops: from TWRP or the USB shell, remove the conf
  file (and put the previous `snd-zrn-abox.ko` back from `audio/modules/` of the
  last known-good build if needed), then reboot.
- The codec is left in reset when the module unloads; LDO34 is left on if the
  driver turned it on (harmless; a reboot clears it).

## 5. Risks

- **Codec supplies.** LDO34 (DCVDD) is not always-on in the stock DT. If
  s9p-acpm is not loaded and the bootloader left LDO34 off, the ID check fails
  (safe failure). Switching LDO34 on is the same PMIC write zrn-bbd already
  does for LDO37.
- **SPI pin roles are inferred**, not measured: Exynos USI order CLK, MOSI,
  MISO, CS on gpp0-4..7 (stock `spi2-bus` lists 6, 5, 4; the same pins are
  `uart3-bus` RXD/TXD/RTS/CTS). If MOSI and MISO are swapped, both sides drive
  one wire only while CS is low (64 bit times, about 0.3 ms): a short logic
  conflict, not expected to damage anything, and the ID check stops there.
- **ABOX writes are new**: UAIF0, NSRC0, SPUM_CTRL0 bits 4/7, QCHANNEL bit 2,
  CMU UAIF0, gpb0 pins. All are what Samsung's driver does for this path and
  none touch UAIF1/SIFS0/RDMA, but this driver has a history of hard hangs when
  ABOX state changes under a running stream. They happen only at capture
  prepare/close.
- **Firmware behaviour unverified**: whether calliope accepts INMUX0 config and
  a WDMA1 stream without other stock state (e.g. `ABOX Sampling Rate Inmux0 Min`,
  `Auto Config Inmux0`) is not proven. If the DMA never runs, the pointer goes
  XRUN after 50 stalled reads rather than hanging.
- **The ASRC on NSRC0 is on** (stock default). The DSP had a "runs 2.8x fast"
  converter bug on playback; if capture rates look wrong, try
  `sfr_field(ABOX_SPUM_CTRL0, 4, 0x1, 0)` (ASRC off: input and DMA share the
  clock here, so it is not needed).
- **cap_wdma=1 is inferred** (capture_link 9 and the path name). `cap_wdma=0`
  is there if WDMA1 turns out wrong.
- **No suspend support** for capture: sleep is refused while recording.
- **Rev A patch only**: the 32-bit `cs47l92_reva_32_patch` (DSP/output
  registers above 0x3000) is not applied; nothing in this path uses them.
- **Stock uses one mic** (DMIC1, copied to both channels, through LHPF1). This
  gives two (DMIC1 left, DMIC3 right). If the right channel is silent or noisy,
  that mic is not where the routing says; the left one is the stock main mic.

## Unknowns to settle on the phone

1. LDO34 state at boot (step 2).
2. Codec ID read (pin roles, supplies).
3. Whether the DSP reports capture periods by GIC line 9, by mailbox, or both
   (`cap_irqs` / `cap_msgs`).
4. Whether WDMA1 runs at exactly 48 kHz with the ASRC on.
