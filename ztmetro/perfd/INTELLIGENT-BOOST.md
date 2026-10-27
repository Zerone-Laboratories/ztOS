# ztOSIntelligentBoost on the SCORE DSP (plan)

zrnperfd today: the tickle system (freezes background apps) and one dynamic boost ladder
(idle0 / nominal / interactive / big / rapid). The design splits the ladder in two:
- **ztOSRapidBoost**: the existing reactive ladder (touch, motion, frame pressure), kept fast and simple.
- **ztOSIntelligentBoost**: picks a whole preset (CPU cores online per cluster, CPU ceilings,
  GPU ceiling/floor) for what is running, learned from measurements, inside the hard limits
  (big cores 2704/2314/1794 MHz by count, efficiency-core priority, thermal caps).

## The accelerator: SCORE (Exynos 9810 vision DSP, 0x16D00000)
- master core + two kernel cores (firmware: score/<set>/score_{mc,kc1,kc2}_{pmw,dmb}.bin in the
  stock vendor: sets camera1-3, develop1 is a dummy); up to 534 MHz (QoS 534/467/336/168/34 MHz);
  own IOMMU (0x80000000-0xF0000000 window), dma-coherent, power domain pd 0x13, clocks dspm/dsps.
- Samsung's libscore.so has `score::MatMul(ctx, A, B, C, M, N, pivot)` = kernel 0x17 (pivot 1/2),
  enough for MLP layers and im2col convolutions. Buffer formats: per-channel bit widths packed in a
  format word (4 x 6 bits). Which firmware set carries kernel 0x17 is not known yet.
- Driver: Samsung's GPL driver in xxmustafacooTR/exynos-linux-stable drivers/vision/score
  (~10.5k lines, hardware/v2): port it as a module (zrn-score) to 7.1, as zrn-modem was.
- Userspace: libscore.so is bionic; write a small C client of the driver's ioctls
  (SCORE_IOC_BOOT, SCORE_IOC_REQUEST[_NONBLOCK], SET_DVFS) and the sc_packet / kernel-header format
  (strings in libscore.so; decode with REA).

## Status 2026-10-25
Steps 1 and 2 are done; see score/README.md. MatMul is exact: 32-bit Q16, C = (A·B) >> 16. The
cost is about 1 ms per request plus about 50 MMAC/s. A 128-input, 64-unit layer for one sample
takes 1.36 ms on SCORE. scoremm's naive scalar check loop (int64, a format switch per element)
does it in 0.17 ms on cpu0, a little core. 64³ takes 6.6 ms on SCORE and 5.2 ms naive on cpu0.
So SCORE does not pay off for IntelligentBoost-sized nets at this clock. Next: measure and raise the SCORE clock, then compare
energy against a little core (the DSP can run while the CPU cores sleep).

## V1 (built 2026-10-25)
- **Log** (`perfd/iblog.c`): every 250 ms with the screen on, one line in
  `/var/lib/zrnperfd/ib/log.csv`. It records the app, the front app's and the shell's load, the
  rung, the profile, touch and plugged-in state, and per cluster the load, clock, ceiling and
  cores online. It also records the GPU clock, the battery's current, voltage and temperature,
  and the model's choice (`ib_rung`, -1 without a model). Buffered, written once a minute,
  rotated at 32 MB.
- **Model** (`perfd/ibnet.c`): 14 inputs → 24 ReLUs → 5 rungs, in Q16.
  - Both products run on SCORE (zrn-score, loaded at boot by
    `/etc/modules-load.d/zrn-score.conf`); the biases and the ReLU run on the CPU.
  - The CPU runs the same integer maths as the fallback. Every 64th SCORE answer is checked
    against it; a mismatch or a failed request means a minute on the CPU.
  - Measured: 4,000 of 4,000 answers bit-exact, about 1.7 ms each (both layers plus the check).
- **Modes**: `echo "ib off|shadow|on|reload" > /run/zrnperfd.ctl`, saved across restarts. The
  status file shows `ib`, `ib_engine` and `ib_rung`.
  - shadow (the default): the model's choice is only logged.
  - on: raise-only. When the model's predicted rung is higher than the current one, the rung goes
    there at once instead of waiting out the ladder's hold times. Only the ladder steps down.
    Never above the profile's top; the clock caps still hold.
- **Training** (`perfd/ib/train.py`, on the laptop): `train.py log.csv... -o model.bin`.
  - Label: the highest rung the ladder itself reached in the next 2 s. The first label tried,
    the rung the next 2 s of load asked for, said rapid 45 % of the time where the ladder ran it
    2.4 %.
  - Samples just before a change of rung weigh 20×, since they are 2 % of the data.
  - Rungs are class-balanced; the last 20 % is held out.
  - The script compares the Q16 integer model with the float one and with the ladder as it
    ran, then writes `model.bin`. Copy it to `/var/lib/zrnperfd/ib/` and send `ib reload`.
  - `--synthetic` tests the pipeline only; that data is random and teaches nothing.
- **SCORE rounding**: each product is floored on its own (`(a·b) >> 16`) and the sum kept in
  32 bits. The CPU path and train.py do the same.
- **SCORE clock** stays at 33 MHz: see the memory note on the lockup and score/README.md.

## Steps
1. Port the driver (power domain, clocks, IOMMU, firmware load); boot the firmware sets; log.
2. Client: one MatMul request on known matrices, compare with the CPU; find the set with 0x17,
   the supported formats and the latency (wake, run, sleep) and energy.
3. IntelligentBoost: features (per-cluster load, frame time / missed vsyncs, front app, temps,
   battery, screen), a small MLP choosing among presets; trained from zrnperfd logs on the laptop;
   inference on SCORE when it pays (batched candidates, or always-on), CPU fallback otherwise.
4. Integrate into zrnperfd: IntelligentBoost proposes, RapidBoost and the safety caps dispose.

## First model (2026-10-25)
Trained on 56k samples (about 4 h, mostly on battery: Waydroid, Superscan, kitty, WhatsApp, Start).
On the held-out fifth:
- before the ladder climbs (202 samples), the model is already at that rung 56 % of the time, with
  no overshoot;
- overall it sits above the ladder 3.8 % of the time and below it 6.5 %. Running raise-only, the
  low guesses do nothing.

Trade-off by transition weight: 5 gives 31 % of climbs early, 1.4 % over; 20 gives 56 %, 3.8 %
over; 60 gives 82 %, 10.8 % over.

## V1 overheated the phone; V2 (2026-10-25 evening)
V1 ran the phone hot, in normal use and on the charger. After one boot it had spent 37 % of the
time at rapid (2.4 % in its training data), with 88 lifts. The cause was a feedback loop. Four of
V1's inputs echoed zrnperfd's own decision: the rung, both clusters' clocks and the big cores
online. Once V1 lifted the rung to rapid, it saw rapid and predicted rapid again. When the ladder
stepped down after 2 s, V1 lifted it straight back.

V2:
- **Inputs (12), demand only:** front-app load, shell load, little-cluster load, big-cluster load,
  touching, Android app, charging, front load 1 s mean, shell load 1 s mean, big-cluster load 1 s
  mean, front load 2 s peak, front load rising (now minus the 1 s mean). Nothing zrnperfd sets.
- **Training:** only the clean log from before V1 went live; transition weight 5. Rows where V1
  held rapid itself are dropped.
- **Decision:** raise-only, to the highest rung the model is at least 80 % sure is needed,
  P(rung ≥ k) ≥ 0.8 (`LIFT_CONFIDENCE`). Its plain best guess sat above the need 12 % of the time.
  On held-out data it catches 33 % of the ladder's climbs early and lifts above the need 2.2 % of
  the time. Stepping down early was checked and rejected: the confident drops went below the need
  99 % of the time.
- **Thermal limit (rule, in zrnperfd):** the hottest of big, little and GPU (from s9p_cpufreq's
  `temp_*` parameters) caps the rung, whatever the ladder or the model asks. 70 °C means no
  rapid, 76 °C interactive at most, 82 °C nominal; each level lets go 3 °C below its mark.
  Performance mode and compile boosts are capped too.
- **Logging:** the log now records `t_big, t_little, t_gpu`. The battery's `temp` is a constant
  34.2 °C and useless, so V3 can learn heat from these columns. The old-format log moved to
  `log.csv.old`.
- **Engine:** the processor (4 µs); SCORE (0.23 ms of kernel time, zero-copy) stays selectable.

## Power limit (2026-10-26)

Asked for: keep the battery draw under 1000 mA in normal use. zrnperfd reads
`max170xx_battery/current_now` every 250 ms on battery (off when plugged in
or in the performance profile), averages it (1/4 per sample, about a second),
and moves a power level, like the thermal limit:

| level | ceiling |
|---|---|
| 1 | no rapid |
| 2 | interactive at most |
| 3 | nominal at most |
| 4 | and the GPU at 60 % of its top |
| 5 | and the efficiency cores at 1.15 GHz |

A level is taken after 0.75 s over 1000 mA, and another every 0.75 s while it
stays over; one is given back after 5 s under 850 mA. The ceiling is the lower
of the thermal and power ones (`limit_top`) and binds the ladder and
IntelligentBoost alike. `/run/zrnperfd.ib` shows `power_ma`, `power_level`,
`power_top` and `power_takes`; the journal logs each change ("power limit N").
The display, modem and radios are outside its reach: with them alone over
1000 mA, it sits at level 5.
