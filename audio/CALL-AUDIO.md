# Call audio on the Galaxy S9: what it takes

Status 2026-10-25: researched, not started. Calls connect and are silent.

## How the stock phone does it

- The voice DSP is not in the modem: **VSS**, a firmware image in RADIO's TOC (cbd already
  loads it into CP shared memory), **runs on ABOX** next to calliope. Shared memory per stock DT
  `shmem,cp_size = 0x7900000`, `shmem,vss_size = 0x700000` from base `0xF0000000`, so VSS is
  `0xF7900000..0xF8000000`. `abox_vss` (magic_offset `0x600000`) zeroes the word at
  VSS + 0x600000 at probe.
- Samsung's abox.c maps VSS into the ABOX IOMMU at IOVA `0xA0000000` (`IOVA_VSS_FIRMWARE`) and
  the VSS parameters at `0xA1000000` (`IOVA_VSS_PARAMETER`, `shm_get_vparam_base()`; location still
  to find). snd-zrn-abox maps neither today.
- The HAL tells calliope the mode: IPC_SYSTEM `ABOX_SET_MODE` with `MODE_IN_CALL`, and
  `sound type` (VOICE / SPEAKER ...) (abox.c `abox_audio_mode_put_ipc`, `abox_sound_type_put_ipc`).
  For a call the INT frequency is raised (`abox_vss_notify_call`: 178 MHz, 400 MHz on speaker).
- mixer_paths.xml (stock vendor), `incall-handset`: SIFS0 -> UAIF1, SIFS0 -> RSRC0 -> WDMA0, and
  `route-handset` = MAX98512 `One Stop Mode: Receiver Right`. **The earpiece is driven by the
  same MAX98512 amplifiers snd-zrn-abox already runs** (left/right amp switched to receiver
  mode). `incall-speaker`: SIFS0 -> UAIF0, amp reference loop, `Mono Left`.
- Microphones are on the CS47L93 codec (`route-main-mic`: AIF1TX1/2 from IN3L/IN3R, DMIC1/3),
  reached over UAIF0. No codec driver is loaded today (mainline: madera / cs47l92 covers CS47L93).
- RIL to modem (libsec-ril, `IpcProtocol41`, decoded with REA):
  - `IpcTxSndSetAudioPath(path, x)`: IPC `09 03`, type 3 (set), 2 bytes `[path, x]`.
  - `IpcTxSndSetVolume(dev, vol)`: IPC `09 01`, type 3, `[dev, vol]`.
  - `IpcTxSndSetClockCtrl(on)`: IPC `09 09`, type 1 (exec), `[on]`.
  - `IpcTxCallRelease()`: IPC `02 03`, type 1, no data (used by ztoscorenetworkingd for hangup).
  Path numbering: still to read from `SoundManager::DoSetAudioPath`.

## Plan

1. Downlink (hear the caller), earpiece and speaker: snd-zrn-abox maps VSS + parameters, writes
   the VSS magic, offers "Audio Mode"/"Sound Type" controls and the SIFS0 -> UAIF1 route with the
   amps in receiver or speaker mode; the modem daemon sends sound clock on + audio path when a
   call becomes active and asks the driver for IN_CALL; back to NORMAL at the end.
2. Uplink (they hear us): CS47L93 on SPI (stock DT node for pins, reset, supplies, MCLK),
   mainline madera driver as a module, mic route into UAIF0.
3. Mute, speaker toggle and volume in ztphone wired to the above.

Risk: kernel module changes can hang the phone; `panic=5` restarts it, and `zrn-audio` skips a
module that stopped the kernel last boot.
