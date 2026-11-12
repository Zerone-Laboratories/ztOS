# The modem on the Galaxy S9 under mainline: what it takes

The modem runs: the SIM is read, the phone registers on the network, and an
SMS has been sent (2026-10-12). What was found, the plan, and where it stands.

## What it is

Samsung's Shannon 360 ("ss360ap" in the stock device tree, driven by the
`ss310ap` code), built into the Exynos 9810. No USB, no serial: the modem and
the application processor share memory, and poke each other through a
mailbox.

| Piece | Where | State under mainline |
| --- | --- | --- |
| Shared memory, 146 MB at `0xF0000000` (+ 32 MB CP log at `0xF9200000`) | stock DT `modem_if`, `cp_ram_logging` | already reserved in our device tree |
| Mailbox `mcu_ipc@14170000` (`samsung,exynos-shd-ipc-mailbox`) | vendor `drivers/misc/mcu_ipc` (GPL) | no driver |
| Modem interface: boot, power, SIPC5 framing, the `umts_*` and `rmnet*` devices | vendor `drivers/misc/modem_v1` (GPL, ~13 000 lines used) | no driver |
| CP power and reset | PMU registers, sequences in `cal-if/exynos9810/pmucal_cp_exynos9810.h` | to be done by our driver |
| CP secure boot | SMC calls into the firmware (`CONFIG_CP_SECURE_BOOT`) | to be done by our driver |
| Modem firmware | `RADIO` partition (`stock/RADIO.img`, 50 MB) | untouched |
| Calibration and identity (IMEI) | `EFS`, `CPEFS` partitions (`stock/EFS.img`, `stock/CPEFS.img` are backups) | untouched |
| Boot loader for the modem | `/vendor/bin/cbd -d -tss310 -bm -mm -P .../RADIO -n /mnt/vendor/efs` | runs in `/run/android` |
| Radio daemon | `/vendor/bin/hw/rild` + `libsec-ril.so` | needs Android's binder, which our kernel does not have |

The stock kernel's devices, from its device tree (21 of them): `umts_boot0`
(firmware download), `umts_ipc0`/`umts_ipc1` (commands, one per SIM),
`umts_rfs0` (the modem reading its files in EFS), `umts_csd`, `umts_router`,
`umts_dm0`, `umts_ramdump0`, `umts_cass`, `umts_ciq0`, `smd4`, `multipdp`,
`multipdp_hiprio` and `rmnet0`..`rmnet7` (data).

## Plan, as for the sensor hub

1. **Kernel side as one loadable module** (`zrn-modem.ko`): mcu_ipc, shm_ipc
   and modem_v1 from the vendor kernel with a compat header and a board file
   in place of the device tree (the `sec_modem,modem_pdata` node and the 21
   I/O devices from `stock/twrp-live.dts`), PMU sequences for CP power, and
   the SMC calls. Loaded behind a mark, as the others are.
2. **Boot the modem** with the vendor's own `cbd` in `/run/android`: it reads
   `RADIO`, pushes the image through `umts_boot0` and starts the CP. Success
   is the modem's first messages arriving on `umts_ipc0`.
3. **Speak to it.** Two ways, to be chosen once step 2 shows what the modem
   says:
   - our own daemon speaking Samsung's IPC on `umts_ipc0` directly (as the
     sensor daemon speaks SSP), and serving `umts_rfs0`;
   - or the vendor `rild`, which needs binder in the kernel (not built in:
     a rebuild, or an out-of-tree module) and a HIDL client.
4. Calls, SMS, data (`rmnet*`), and the status bar's signal bars.

## Cautions

- **EFS holds the phone's identity.** `cbd` and the modem write there. Work on
  a copy first; the partitions are backed up in `stock/`.
- A wrong PMU or mailbox write can stop the kernel, as with the idle driver:
  the module is developed over the USB link, never loaded at boot until it
  has run clean.
- The modem asks for bus and CPU speeds through the mailbox
  (`mbx_cp2ap_dvfsreq_*`); with no DVFS for MIF/INT under mainline these are
  acknowledged and ignored at first.

Sources: `driver/` holds the vendor copies, unmodified; `zrn-modem/` the port.

## Where it stands (2026-10-12)

`zrn-modem/` builds `zrn-modem.ko` against our 7.1 kernel with no unresolved
symbols. In it:

- the vendor's `mcu_ipc`, `shm_ipc` and `modem_v1` files (stock options, from
  `stock-kernel.config`, extracted from the stock boot image), with
  `zrn_compat.h` for what mainline renamed or dropped (time, timers,
  wakelocks on wakeup sources, pm_qos accepted and ignored, the secure
  monitor through `arm_smccc_smc`) and `zinc/` for missing headers;
- `zrn_cp_pmu.c`: the CP's PMU sequences (`cal_cp_*`) from Samsung's PMU CAL,
  the two CP_CTRL registers through the secure monitor;
- `zrn-modem-overlay.dts`: the stock tree's mailbox, shared memory and modem
  nodes (21 io devices), applied as an overlay when the module loads, so the
  boot image does not change (`mkoverlay.sh` regenerates the blob);
- the modem memory found in our device tree at load, and mapped with
  `ioremap_wc` (ours is no-map; the vendor vmapped struct pages).

Changes from stock: zero-copy receive off (needs `__build_skb`, not exported);
ACPM's dump region optional; the CP log device only unregistered when it
exists (unloading oopsed without that).

On the phone (`insmod`, after a fresh boot), with no warnings: mailbox and
shared memory up, "CP memory map verification: PASS" (5 binaries in the layout
stock has), the ss360ap control device, the shmem link (SBD IPC, buffers at
the stock offsets), and the io devices: `umts_boot0`, `umts_ipc0`/`1`,
`umts_rfs0`, `umts_dm0`, `umts_router`, `umts_csd`, `umts_cass`,
`umts_ramdump0`, `multipdp`, `multipdp_hiprio`, `rmnet0`..`rmnet7`.

Not loaded at boot yet. Do not unload it by hand while unsure of a remove
path: the first build's oops left it stuck, and the phone then could not shut
down (it hung stopping the sensor hub) and had to be forced off.

## Step 2 and 3: the modem running, SIM, network, SMS (2026-10-12)

`/usr/local/sbin/zrn-modem` (rootfs/overlay) does what stock init did: loads
the module, gives `/dev/umts*` their stock owners (system:radio 0660) and the
RADIO partition radio read access, mounts the phone's EFS at
`/mnt/vendor/efs` inside the Android root as stock does, and runs the vendor's
`cbd` with its stock command line and groups. It is not started at boot yet.

`cbd` reads the TOC of RADIO (BOOT, MAIN, VSS, NV), sends the images through
`umts_boot0`, the secure monitor verifies and starts the CP (CP_NO_ERROR),
the boot handshake passes, the CP sets up 9 SBD ring buffers and goes ONLINE.

Two fixes on the way: the zero-copy region was reached through the linear
map (no-map here: it is now ioremapped, and its DMA calls are plain
physical addresses); and `zrn-android-root` stacked a new root on every
service restart (its check followed a link that only resolves inside).

Talking to it:
- Samsung IPC on `umts_ipc0` (`ril/zrn-ril.py`): status queries answer
  (SIM ready, NET_REGIST, serving network 41302 Dialog, RSSI), but this
  modem rejects the older layouts for SMS and raw SIM access (0x8001).
- AT commands on `umts_router` (`ril/at.py`): `+CPIN: READY`,
  `+COPS: 0,0,"Dialog",7`, `+CREG: 1`, `+CGREG: 1`, `+CSQ: 19`; the SIM's
  SMS centre `+9477000003`. Text-mode `AT+CMGS` fails (`+CME ERROR:
  unknown`); PDU mode works: `ril/send-sms.py NUMBER TEXT` → `+CMGS: 21`.

The modem's RFS requests (its NV, which stock saves to EFS) are answered
since 30 Oct, without storing anything: see "The CP's IPC going silent"
below. Nothing is written to EFS.

## The shell's side (2026-10-12)

`ztmetro/modemd/zrn-modemd.c` (root, `zrn-modemd.service`, after
`zrn-modem.service` which runs cbd; both enabled at boot) owns the AT port,
keeps `umts_ipc0`/`umts_rfs0` open, and writes `/run/zrn-modem/status`
(sim, operator, bars 0..5, registered, tech, airplane, data, wifi_bars),
poking the shell's control pipe ("modem") on any change. It takes
`airplane on|off`, `data on|off` and `sms NUMBER TEXT` on
`/run/zrn-modem.ctl`. ztMETRO draws the bars, the data type beside them and
the operator's name from that file.

Mobile data works (2026-10-12): 4G, about 300 KB/s on a first try, `rmnet0`
with the carrier's address and name servers, default route at metric 700
(Wi-Fi wins while it is up). `zrn-modemd` brings it up whenever `data on` was
asked (`/var/lib/zrn-modem/data_on`), the modem is registered and no context
is up.

How it is done, through Samsung IPC on `umts_ipc0` in the layouts
`libsec-ril.so` builds (`IpcProtocol41::IpcTxDefinePdpContext`,
`IpcTxSetPdpContext`; the library has symbols, so they can be read off):

- `0d01` set, 127 bytes: `1, cid, type (2 IP), apn[101], v6[16], v4[4], u16 0, u8 0`
- `0d04` set, 105 bytes: `state (1 up, 0 down), cid, flags u32 (1 user,
  2 password, 0x10 auth), user[32], password[32], 32 x 0, auth, 0, reason`
- each answered by `8001` (`0x8000` is success); then `0d10` (cid, state:
  3 on its way, 1 up) and `0d09`, 257 bytes: `cid, flags u16, address[4],
  dns1[4], dns2[4], ...`. Context 1 is `rmnet0`.
- The access point is `/etc/zrn-modem/apn` (`apn`, `user`, `password`,
  `auth`), written by Settings > cellular + SIM > access point; `apn` on the
  control pipe reconnects with it. Default `dialogbb`.

What had to be fixed in the driver port for it:

- **The two shared data rings were the wrong way round.** All rmnets share
  two rings, high priority and normal; stock told them apart by their order
  in the device tree. Our nodes come from an overlay, which lists them in
  reverse, so the big zero-copy ring was taken for the high priority one:
  nothing was received and the modem stopped (`CP_CRASH_EXIT`) when it wrote
  a downlink packet. Now by name (`init_ctrl_tables`).
- **Receive copies.** The zero-copy path built skbs over the ZMB region with
  `build_skb`; that region is ioremapped no-map memory here, with no struct
  page, and the kernel stopped at the first packet. `sbd_pio_rx_zerocopy_adaptor`
  always takes the copying branch now.

Guards in the daemon, from how it went wrong on the way:

- a mark (`/var/lib/zrn-modem/data_trying`) from the first request until the
  context has been up 90 s; found at start, the wish is switched off, so a
  fault in the data path cannot restart the phone for ever;
- two requests with no answer and the modem is restarted (`systemctl restart
  zrn-modem`, no reboot needed, about 20 s): after this daemon has been
  restarted the modem stops answering requests, cause unknown;
- a context that has sent five packets and received none after 20 s is taken
  down and brought up again (seen after a modem restart).

Never by AT: `AT+CGDCONT` + `AT+CGACT=1,1` answers OK, brings nothing up and
knocks the modem off LTE until `AT+CFUN=4`, `1`. `AT+CFUN?` has been seen to
answer "off" while registered; the daemon does not call that airplane mode.

`/var/lib/zrn-modem/debug` makes the daemon log every IPC message (not the
ones carrying the SIM's or the phone's own numbers). `ipc MAIN SUB TYPE [HEX]`
and `at COMMAND` on `/run/zrn-modem.ctl` send one raw message.

**The board revision.** With any traffic at all the modem used to lose LTE
packet service within seconds, fall to 2G (E/G) and climb back, over and
over; idle, it held 4G. The modem was being told board revision 0: stock's
driver reads the revision from eight pins (gpg3-3..0, gpg1-5..2) named in its
device tree, and those references were dropped from our overlay. The modem
picks its radio's settings by that number. Read on this phone
(`modem/ril/hwrev.py`) the pins give 174, now in the overlay as
`zrn,sys_rev`; cbd's `model_info-system_rev` ("026", as stock's tree has it)
is added to the root too. After that: 10 MB at 558 KB/s without leaving 4G,
at one bar.

Not done: IPv6 (the context is IPv4 only, and IPv6 is switched off on the
interface), the APN from the SIM, roaming rules, data counters.

Next: the IPC layouts for packet data (and SMS, SIM access) from how the
vendor library builds them; receiving SMS; calls and audio; an SMS app.

## The CP's IPC going silent, and the file requests (2026-10-30)

**Answer the CP's file requests and its IPC stays alive.** In every boot
until now the CP stopped sending IPC about 100 s after its start (the last
message `0f36`, SAR control, which wants no answer): later 0501 probes went
unanswered and the screen-state message 0a0f did nothing, while AT kept
working. ztoscorenetworkingd now answers RFS (`umts_rfs0`) the way stock's
libsec-ril does, and the CP keeps answering.

What the CP asks at its start (observed, journal): `2601` (OesDrkCpInfo
indication), `0407` (SMS device ready), and on `umts_rfs0` an OpenFile of
`CP_OMC_AUDIO_SLSI.bin`, a CloseFile, an OpenFile of `CP_AUDIO_SLSI.bin`,
NvWrites (offset 0; 97 626 and about 20 300 bytes, arriving as 2040-byte
reads with the link header already gone), `FLL.bin`,
`err/csdiag_rf_Mipi.dat`, `err/csdiag_nosvc_Info.dat`; then more NvWrites
every minute or two.

The answers, read from libsec-ril.so (sha256 4fa6207a...ccdf, REA/Ghidra
12.1.2; Ghidra addresses are objdump's + 0x100000):

| request | stock handler | answer here |
|---|---|---|
| RFS 0x02 NvWrite: offset u32 @6, size u32 @10, data @14 | `Nv::ProcessNvWrite` 0x2d9590 (ev_f221632b...): writes nv_data.bin, then 15 bytes `u32 0x0f, 0x02, id, ok (1 on success), u32 offset, u32 written` | the same 15 bytes, ok 1, written = size; nothing stored (cbd loads the real nv_data.bin at the next start) |
| RFS 0x11 OpenFile: flags @6, path length @10, path @14 | `Nv::ProcessOpenFile` (ev_a2d9d7f4...): 14 bytes `u32 0x0e, 0x11, id, i32 fd, u32 errno` | fd -1, errno 2 (ENOENT) |
| RFS 0x06 CloseFile: handle i32 @6 | `Nv::ProcessCloseFile` 0x2da2e0 (ev_e1bb80fd...): 14 bytes `u32 0x0e, 0x06, id, i32 result, u32 errno` | -1, errno 9 (EBADF): nothing was opened |
| other RFS commands | `Nv::ProcessRfsMessage` 0x2d9218: 0x01-0x17, each its own layout | logged, not answered |
| IPC 2601 type 1 | `IpcTxCfrmOesDrkCpInfo` 0x4503c8 (ev_b13e084e...): `07 00 00 00 26 01 04` | 2601 type 4, the indication's sequence number as aseq |
| IPC 0407 type 3 | `IpcTxSetDeviceReady` 0x447648 (ev_b13e084e...): `07 00 00 00 04 07 03` | 0407 set, **once a modem start** |
| IPC 0402 (an SMS) | `IpcRxIncomingMsg` 0x4480e8 (ev_1b941abc...): data[0] network (2 GSM), data[5] length, data+6 SMS centre + SMS-DELIVER | decoded and logged as one read by AT; no delivery report (0406, layout not read) |

How each was tested (30 Oct, phone on the charger, screen off, the debug
file present so that a 0501 probe goes out with every poll). Isolation
runs restart only the modem (`systemctl restart
ztoscorenetworking-modem.service`, about 8 s to the CP's 0101): restarting
the daemon leaves the modem deaf to requests (above).

| run | answers on | how | the CP's IPC after t0+100 s |
|---|---|---|---|
| control | none | reboot | silent: last `ipc <- 0f36` at t0+100 s; probes at t0+175, 295, 415 s unanswered |
| R0 | none | modem restart | silent: last message at t0+55 s; probe at t0+112 s unanswered |
| RA | rfs, cfrm, smsready | modem restart | not a result: answering each 0407 made a loop (the CP answers our 0407 with 8001, 040c and a new 0407): 70 386 sets in 4 min, about 330 a second, stopped by the next modem restart. Fixed: once a start |
| RA' | rfs, cfrm | modem restart | alive: probes at t0+40, 160, 280, 400, 520 s answered |
| RB | cfrm | modem restart | silent from t0+45 s: probe at t0+114 s unanswered, `ipc: CP silent since 1807s` |
| RC | rfs | modem restart | alive: probes at t0+65 ... 545 s answered, frames to t0+630 s |
| T3 | rfs (the CloseFile answer added) | reboot | alive for 30 min: all 14 probes from boot+196 s to 1756 s answered; no restart, no replies-off; `umts_rfs0` flat from boot+174 s to the end |
| after a reset | rfs, set again with `replies` | modem restart, in a boot where the CP had already gone silent | alive again: probes at t0+111 and t0+231 s answered |

So the RFS answers are what keeps it alive (observed, 3 runs with them,
3 without). Why (inferred, not read in the CP's code): the CP's IPC task
waits on its NV writes being acknowledged. 0f36 is not the cause; it is
only the last message before the stall.

The CP's NvWrites go on after its start, every minute or two while the AP
is awake (they are real NV saves, not retries: each is a fresh size, and
stock writes each one to EFS). With the debug probe on, many come within
60 ms of a 0501 probe's answer (inferred: the probe makes the CP save).

Flags and recovery:

- `/etc/ztoscorenetworking/reply-rfs`, `reply-cfrm`, `reply-smsready`: an
  answer is given while its file exists. Read at the daemon's start, at
  every modem start (0101), and on `replies` on `/run/ztoscorenetworking.ctl`.
  The CP's first OpenFile comes just before its 0101: after changing the
  files, send `replies` before restarting the modem.
- `/var/lib/ztoscorenetworking/replies-off`: no answers at all. Written by
  the crash guard when the modem starts again within 300 s of our first
  answer without the daemon having asked for it (an answer within 5 s
  before the 0101 belongs to that start), or when the driver refuses two
  answers in a row (EPERM, ENODEV); the shell gets a notification.
- The phone runs with `reply-rfs` only (the smallest set that passed).
- Every request is logged on one line (`rfs: request 0x02 id 0, 20310
  bytes -> replied`), command, id, length and an opened file's name only,
  at most 40 a modem start. No SMS payload (main 04) is hex-logged any more,
  and EFS is never opened: nv_data.bin's md5 was the same before the
  runs, after the first reset and at the end.
- `ipc=alive|silent` and `ipc_last_rx=` in /run/ztoscorenetworking/status
  (the probe goes only while the debug file exists; 3 unanswered in a row
  log `ipc: CP silent since <t>s`).

In SoC sleep, with the IPC alive (30 Oct, `once 300` on the charger): three
full sleeps of 297 s ended by the RTC, with no modem wake and no RFS wake
(one `umts_ipc0` frame arrived in one sleep's window without waking it).
Awake, the screen-state message does not hold back the CP's signal
indications: with the screen off (T3, 26 min) the CP sent 151 messages, all
0706 (signal), 5.8 a minute; with the screen on (5 min) 9, 1.7 a minute.
While the AP sleeps they did not come (inferred: modem_suspend tells the
CP, pda_active 0, and it holds them).

Not known yet: whether an SMS left without a delivery report (0406) is
sent again (a PDU seen in the last day is ignored either way,
`/var/lib/ztoscorenetworking/sms/seen-*`); a real call and SMS from SoC
sleep (J6, needs the user and a second phone); the unplugged night (J5).
