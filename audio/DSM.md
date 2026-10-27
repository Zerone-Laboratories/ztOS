# Samsung's speaker tuning (Maxim DSM) on ABOX: what stock does

Collected 2026-10-25 from the stock DT (stock/twrp-live.dts), the stock vendor (mixer_paths.xml,
libaudioproxy.so via REA), Samsung's kernel sources (third_party/vendor-kernel, and
xxmustafacooTR/exynos-linux-stable) and the phone's EFS. Status: not running yet.

## Gains (stock DT max98512)
- bottom speaker, right amp 0x3a: `maxim,spk-gain = <0x05>` (+15 dB analogue).
- earpiece, left amp 0x39: `spk-gain-rcv = <0x01>` (+3 dB), `digital-gain-rcv = <0x34>` (-3 dB).
  snd-zrn-abox uses these for the earpiece now (EAR_GAIN, EAR_DIGITAL); the bottom speaker is at
  +12 dB, 0 dB digital, behind PipeWire's speaker chain, until DSM runs.
- `maxim,vmon_slot = <0>`, `mono_stereo = <1>`, `sub_reg = <0x3a>`.

## Calibration (factory, per phone) in EFS `/efs/maxim/` (on ztOS: /run/android/mnt/vendor/efs)
`rdc_cal` 0xeb460 (left), `rdc_cal_r` 0xe9c0e (right), `temp_cal` 0x13a. Text, hex.

## DSP side
- extra firmware in ABOX DRAM (loaded by snd-zrn-abox since today): dsm.bin @0x502000,
  dsm_tune.bin @0x601000, SoundBoosterParam.bin @0x4fc000, AP*_AUDIO_SLSI.bin.
- DSM on/off: IPC_ERAP REALTIME_OPEN/CLOSE, type ERAP_VI_SENSE, channel 0, version 1
  (abox.c abox_erap_handler_put_ipc; mixer ctl "ABOX ERAP info DSM On").
- DSM parameters: IPC_ERAP REALTIME_EXTRA, raw params [1 (write), offset, count, values...]
  (abox_adaptation.c maxim_dsm_write); 185 params per channel (PARAM_DSM_5_0_MAX), right
  channel at +185; the DSP answers PARAM_DSM_5_0_ABOX_WRITE_CB (0xDAAD). Reads: [0 ...].
  Indices (include/sound/maxim_dsm.h): 1 ENABLE, 6 RDC_AT_ROOMTEMP, 18 ADAPTIVE_DC_RES (get),
  25 ENABLE_RDC_CAL, 28 PILOT_ENABLE.

## Routing for media on the speaker (mixer_paths.xml media-speaker)
SIFS0 mix -> RSRC0 (ROUTE_CTRL2[3:0]=1) -> RECP PIFS0 (SPUM_CTRL1 bit0) -> WDMA0, into DSM;
DSM out on RDMA7 (hostless, pcmC0D7p, 48k/2ch/480x4) -> SPUS OUT7 -> SIFS1
(SPUS_CTRL1[18:16]=7) -> UAIF1 SPK (ROUTE_CTRL0[7:4]=2) -> amps.
VI sense: amps' PCM TX (PCM_TX_CH_SRC, slots) -> UAIF1 MIC (UAIF1_CTRL0 bit1) -> NSRC3
(ROUTE_CTRL1[15:12]=9) -> SIFM3 = WDMA (SPUM_CTRL0 bit19=0) -> WDMA4 (pcmC0D12c, hostless).
