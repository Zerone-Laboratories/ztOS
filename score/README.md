# SCORE (Exynos 9810 vision DSP) on ztOS

Status 2026-10-25: Samsung's GPL driver runs as the module `zrn-score`, the stock camera firmware
boots, and MatMul (kernel 0x17) gives exact results.

## Hardware and firmware
- SCORE at 0x16D00000, IRQ SPI 222. It has a master core (MC) and kernel cores KC1 and KC2.
  The driver has KC2 registers, but the stock firmware sets (camera1, camera2, camera3) ship
  only `score_mc_*` and `score_kc1_*`, so one kernel core does the maths.
- Power domains DSPM/DSPS and the clocks are already on at boot. Both SYSMMUs are off, so the DSP
  uses physical addresses. The clock rate is not measured yet (QoS table: 534/467/336/168/34 MHz).
- Firmware ids: 3 camera1, 4 camera2, 5 camera3 (`/lib/firmware/score/<set>/`).

## Driver (score/driver, module zrn-score)
- A port of drivers/vision/score (v2) to 7.1; `zrn_score_board.c` registers the platform device.
- Copy mode (`score_memory.c`): every buffer is copied into a 16 MB coherent pool before the
  request. At frame completion (`zrn_score_memory_sync_back`) it is copied back, because unmapping
  happens later from the freelist worker, after user space has read the buffer.
- `ZRN_SCORE_IOC_ALLOC` (`_IOWR('S', 32, struct zrn_score_alloc)`): returns a dma-buf fd of N bytes
  of DMA-coherent memory the DSP addresses directly, mmap-able (`dma_mmap_coherent`). This kernel
  has no ION, udmabuf or dma-heaps; Samsung's userspace used ION. The driver recognises these buffers
  (`zrn_score_buf_dvaddr`) and passes their address with no copy; other dma-bufs still go through
  the copy pool.
- Processor time per IntelligentBoost prediction (two MatMul requests, 14→24→5), 2000 predictions
  measured with `time ibtest 2000 score|cpu`:
  - copy pool: 0.42 ms of kernel time;
  - zero-copy: 0.23 ms (wall-clock 1.6 ms, the DSP at 33 MHz);
  - on the processor: 0.004 ms.

  The rest is the per-request path: frame, packet, buffer lookups, writes to the SCQ registers,
  the interrupt and the wake-up. This kernel has no ftrace to split it up.
- Install to /var/lib/zrn-score.ko and `insmod` it. Never rmmod it: reboot instead.

## Request format (decoded from the stock libscore.so)
Host packet (fd from ZRN_SCORE_IOC_ALLOC), given to SCORE_IOC_REQUEST as `packet_fd`:
`{size, packet_offset = 8 + 24*nbuf, version 0x760}`, `{valid_size = data bytes, nbuf}`,
`score_host_buffer[nbuf]`, then the data.

Data = kernel header plus params:
- header: u32 `6 | 1<<13 | 2<<18`, then u64: bits 4..14 = params bytes + 8, bits 15..23 = kernel
  id, bits 24..31 = task id (patched by the driver, host buffer type 2 at offset 7), bits 32..39 =
  priority (2).
- ScBuffer: `[1][width][height][format][device address]`; the address is patched from a host
  buffer (memtype 2 = dma-buf, offset = param start + 16, type 1).
- ScExtBuffer after it: `[3][index | direction<<8][u16 tile w][u16 tile h]`, then optional
  `[9][dfc modes][count]`, `[0xb][tile iterator]`, `[0xc]` (MatMul pivot).
- ScScalar: `[2][index][value]`.
- Format word = `bits<<8 | id`. Seen in stock libs: 0x81c (8-bit), 0x101d and 0x101e (16-bit),
  0x2020 (32-bit).

## MatMul (kernel 0x17)
`score::MatMul(ctx, A, B, C, M, N, pivot)`, which `score/tools/scoremm.c` reproduces:
- A: K wide by N high; B: M wide by K high; C: M wide by N high. All three are 32-bit Q16
  (format 0x2020).
- C[n][m] = Σk (A[n][k]·B[k][m]) >> 16: each product is floored on its own and the sum kept in 32
  bits (checked against fractional Q16 values; whole-number tests can't tell this from one shift at
  the end). Signed and exact against that reference. Tested on 16³, 32³, 64³, 128³, 32x16x64, 16x64x32,
  64x1x128, 64x2x128 and 16x1x32.
- With 8- or 16-bit formats the DSP still multiplies 32-bit words, so the results are scrambled.
  Use 0x2020.

Timing (one blocking request, including the copies):

| M x N x K | MACs | ms |
|---|---|---|
| 16x1x32 | 512 | 1.0 |
| 64x1x128 | 8 K | 1.4 |
| 32³ | 32 K | 1.7 |
| 64³ | 262 K | 6.5 |
| 128³ | 2.1 M | 40.9 |

These timings are at the boot clock, 33 MHz. In CMU_TOP, MUX_CLKCMU_DSPM_BUS at 0x1a24104c is 2,
which selects PLL_SHARED2/2 = 400 MHz, and DIV_CLKCMU_DSPM_BUS at 0x1a241844 is 0xb, a ratio of 12.
The mux parents are 0 = 533, 1 = 466, 2 = 400 and 3 = 333 MHz.

With the clock raised by CMU writes, results stayed exact (128³ / 64x1x128):

| Clock | 128³ | 64x1x128 |
|---|---|---|
| 33 MHz | 40.9 ms | 1.36 ms |
| 67 MHz | 21.4 ms | 0.95 ms |
| 100 MHz | 14.7 ms | 0.75 ms |
| 200 MHz | 7.5 ms | 0.63 ms |
| 400 MHz | 5.2 ms | 0.39 ms |

**Do not do this again.** About five minutes after the 400 MHz run, with SCORE idle, the DSP raised a
CACHE error (core 0x3, ADDR 0xfc000000) and sw_reset failed. The error IRQ then stormed CPU1 into a
hard lockup and the kernel panicked. dvfs_score is an ACPM domain, so the APM firmware has to raise
the voltage together with the clock. To-do: ACPM DVFS for SCORE, and make the error IRQ disable
itself instead of looping.

`scoremm FW M N K [PIVOT] [FMT_IN] [FMT_OUT]`, for example
`SCALE=65536 SHIFT=16 scoremm 3 64 1 128 1 0x2020 0x2020`. Debug environment variables: ONES,
IDENT, PROBE=y,k, PROBEB=b, NZ, DUMP, SAVE.
