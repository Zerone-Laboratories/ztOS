# Superscan browsing freeze investigation — 2026-10-24

## Diagnosis

Reproduced the allocation failure by opening `https://uptodown.com` in Superscan with Waydroid present. Enabling the existing DRM diagnostic (`echo 4 > /sys/module/drm/parameters/debug`) produced:

```
panfrost 17500000.gpu: [drm:drm_gem_shmem_get_pages_locked] Failed to get pages (-12)
MESA: error: DRM_IOCTL_PANFROST_CREATE_BO failed (err=12)
```

This is **ENOMEM**, not ENOSPC. `drm_gem_shmem_get_pages_locked()` returns the failure from `drm_gem_get_pages()` directly; `drm_gem_shmem_get_pages_sgt_locked()` propagates it to the warning in `panfrost_mmu_map()`. This uses the kernel's existing debug print rather than adding a replacement module. The running kernel has neither CONFIG_KPROBES nor CONFIG_FTRACE. DRM debug was returned to zero after capture.

The strongest mechanism is pressure in GPU-addressable memory, amplified by fragmentation and the absence of swap. This port sets `__GFP_DMA32` on **every** Panfrost BO in `build/linux-7.1/drivers/gpu/drm/panfrost/panfrost_gem.c`, so system-wide MemAvailable overstates what a GPU allocation can use. The DMA zone manages about 1,548 MiB; the Normal zone manages about 1,969 MiB. Free CMA pages are also not interchangeable with ordinary non-movable BO backing pages.

THP was enabled on Panfrost's private `huge=within_size` tmpfs mount. However, **a missing order-zero fallback is not the cause**: `shmem_get_folio_gfp()` retries `shmem_alloc_and_add_folio(..., orders=0)` after the huge-page attempt fails. Changing global shmem THP to `deny` still left BO allocation failures before swap was added. THP therefore contributes allocation/compaction pressure, but disabling it alone did not fix this reproduction. An attribution to fragmentation alone would overstate the evidence.

The original Superscan core confirms a separate allocation-error handling failure inside Mesa 26.2.5: `dri2_query_image` → libEGL_mesa → `zmg_end` → `present_gpu` → `xdg_surface_configure`. The old app stopped on a returned EGL failure; the recorded SIGSEGV occurs inside Mesa before EGL returns. Application error checks improve returned-error recovery, but do not establish that arbitrary internal Mesa crashes are recoverable.

## Failure-state numbers

The snapshot at **09:04:45 UTC** was taken during the reproduction, shortly after repeated failures. It is not an atomic snapshot at the exact failed allocation.

| Metric | Value |
|---|---:|
| MemTotal | 3,601,040 KiB |
| MemFree / MemAvailable | 137,464 / 836,700 KiB |
| Shmem / Unevictable | 746,016 / 755,916 KiB |
| AnonPages / AnonHugePages | 1,504,412 / 860,160 KiB |
| ShmemHugePages | 208,896 KiB |
| SwapTotal | 0 |
| CmaFree | 35,796 KiB |
| DMA free | 88.46 MiB |
| DMA free blocks of order 9 or higher (2 MiB+) | 0 |
| Normal free | 45.47 MiB |
| Normal free blocks of order 9 or higher | 3 |

The buddy snapshot and meminfo reads are sequential, so small differences are expected. Existing VM settings were `min_free_kbytes=45056`, `watermark_scale_factor=10`. They were left unchanged: raising reserves is not a demonstrated fix for this constrained zone.

Panfrost debugfs `gems` identifies **722.14 MiB resident**, not the much larger sum of virtual BO sizes. Imported buffers report zero backing here, so attributing by creator avoids counting imports twice.

| Creator | Resident MiB |
|---|---:|
| ztmetro | 120.08 |
| Superscan UI | 47.82 |
| WPEWebProcess | 191.93 |
| Android allocator | 208.33 |
| WhatsApp | 38.18 |
| Android SystemUI | 33.93 |
| Android keyboard | 22.64 |
| Android launcher | 20.34 |
| Android system_server | 18.21 |
| Android composer | 15.15 |
| Android surfaceflinger | 5.54 |

Android creators total 362.31 MiB. Before policy deployment the Waydroid payload cgroup accounted for approximately 1.87 GB, including file cache and shared memory; that is not private RSS.

## Changes deployed on the phone

- `ztos-memory-policy` provides a **512 MiB zram swap device**, priority 100. This kernel supports **lzo-rle**, not lz4; the compressor occupies much less physical RAM than the swapped anonymous pages. Existing configured zram is not reset.
- Global `shmem_enabled=deny` bypasses the private GPU tmpfs mount's THP policy without unloading the active GPU driver. `never` does not override a private mount. This is a broad, reversible runtime mitigation. The narrower boot alternative is `panfrost.transparent_hugepage=0`; it was not flashed or reboot-tested.
- Waydroid's actual `lxc.payload.waydroid` cgroup receives **memory.high=1536 MiB**. `memory.max` remains `max`. This is a reclaim/throttling boundary, not an Android OOM cap or a guarantee of maximum usage. A 60-second timer reapplies it when LXC recreates the cgroup.
- Superscan creates WebKit contexts with a **384 MB pressure reference**, conservative trimming at 192 MB, strict trimming at 288 MB, a two-second poll, and kill threshold zero. These are WebKit cleanup thresholds, not hard RSS/GPU caps. The document-viewer cache policy avoids retaining previous pages. Hidden tabs report visibility to WPE.
- ZrnMetroCore retains page textures within a **64 MiB budget**. One oversized current page is allowed exclusively, so long lists still render. Failed texture uploads and DMA-BUF imports release incomplete resources. A failed GL frame drops page textures before swap; a returned EGL failure also drops caches. Failed presentation retries after 100 ms through the existing poll loop instead of terminating or awaiting a frame callback for a commit that never happened.
- The shell releases failed layer uploads, skips output frames with GL errors, and schedules a 100 ms retry after swapchain/framebuffer allocation failures. The previous scanout remains displayed while retrying. No renderer change or software drawing path was introduced.
- Superscan checks host allocations for imported-image bookkeeping and Cairo fallback images.
- Coredumps use `Storage=none`, `ProcessSizeMax=16M`, `ExternalSizeMax=16M`, `JournalSizeMax=0`. Small dumps can still yield journal backtraces; large applications may produce metadata only. Workers run at nice 19, idle I/O priority, CPUWeight=10 and IOWeight=10. Existing historical dumps were retained.

The shell and browser were built **on the phone** in `/home/zerone/ztmetro-phase2`. Only task-modified sources were copied. Existing unrelated edits in `ztmetro.c`, `zrnmetrocore.c`, browser sources and perfd were retained. Deployment checks DemoLoop before restart, stages files atomically and backs up originals under `/var/lib/ztos-freeze-backup`. No kernel flash or reboot was required.

## Validation

**Result: memory-allocation freeze mitigation passed this workload; full GPU acceptance did not pass.** The final binaries ran from **09:24:03 to 09:34:04 UTC**, 601.21 seconds, with Uptodown, Wikimedia Commons, and Uptodown Android repeatedly scrolled. All three screenshots were inspected.

- Sixty samples found the browser active, shell PID **13962** unchanged, and Waydroid payload **unfrozen** every time.
- **Zero new `Failed to get pages` or `panfrost_mmu.c` allocation warnings.**
- Worst logged shell slow-frame interval: **34 ms**. The ten-second memory sampler's worst shell response was **12.63 ms**.
- An additional **8,286 shell probes at 20 Hz over the final 419.97 seconds** found **58.27 ms worst response**, **zero timeouts**, and **zero responses over 250 ms**. This is sampled event-loop evidence, not a trace of every display frame. An earlier version of this extra probe lost split log lines; its apparent one-second timeouts were a measurement bug. The corrected probe accumulates partial reads, and its results are retained separately.
- Kernel review nevertheless found **three GPU job faults**: WPEWebProcess DATA_INVALID_FAULT at uptime 2201.73; WPEWebProcess JOB_BUS_FAULT plus a translation/page fault at 2296.57; shell DATA_INVALID_FAULT at 2324.84. These were not allocation warnings, and caused no measured shell stall or process restart, but they violate the requested clean-Panfrost acceptance condition. The existing GPU fault class documented in the repository README remains unresolved; this work does not claim to fix it.
- Android init also repeatedly reported hwcomposer exits. Its payload stayed populated and unfrozen, but this test does not establish a healthy Android display stack. Similar Android composer/surfaceflinger crashes predate this task. The soft memory boundary did not impose a hard limit; `memory.max` remained unlimited.

| Final workload memory | Before browser launch | After ten minutes |
|---|---:|---:|
| MemFree | 854.80 MiB | 242.81 MiB |
| MemAvailable | 1,614.69 MiB | 1,051.63 MiB |
| Shmem | 462.15 MiB | 699.92 MiB |
| ShmemHugePages | 116.00 MiB | 116.00 MiB |
| CmaFree | 84.44 MiB | 6.44 MiB |
| Swap used | 409.72 MiB | 480.20 MiB |
| Resident Panfrost BOs | 457.88 MiB | 695.25 MiB |
| Waydroid cgroup charged memory, first/last sample | 1,205.75 MiB | 1,129.27 MiB |

Minimum sampled MemAvailable was **922.94 MiB**. Peak sampled swap use was **481.95 MiB**. The last zram sample held **472.73 MiB** of original pages in **46.59 MiB** of compressed data and **49.73 MiB** of allocated zram RAM. Existing huge shmem pages were not forcibly split: disabling THP affects new allocations.

After verification, the Waydroid launch inhibitor was removed, normal idle policy resumed, and the configured **180-second** screen timeout was restored. The policy service and timer remain enabled; installed shell/browser hashes match the phone builds.

Focused fault injection:

- `GL_OUT_OF_MEMORY` after clear: Superscan logged a skipped GL frame, retried after 100 ms, remained alive and loaded Uptodown.
- A failed texture allocation: Superscan retried the incomplete upload and remained alive.
- An 87.9 MiB page texture rendered successfully, checking the oversized-page exception.
- `EGL_BAD_ALLOC` at swap: Superscan retried and loaded Uptodown instead of exiting.
- `GL_OUT_OF_MEMORY` after shell `glFinish`: shell logged `skipping failed output frame (GL 0x505)` and continued running. The temporary session-launcher injection was restored.
- A deliberately aborted Python process holding 64 MiB produced `Storage: none`; processing completed within the same/next journal second. This tests bounded handling, not an exact subsecond duration. A naturally occurring small Android composer crash also produced a journal backtrace without retained storage.

Fault injection validates error returns at the application boundary. It does **not** simulate every Mesa internal BO failure or prove recovery from a driver SIGSEGV.

## Reproduction and rollback

`freeze/verify.py` runs the phone's ten-minute workload: Uptodown, Wikimedia Commons, and Uptodown Android; repeated touch scrolling; memory/fragmentation/zram samples every ten seconds; shell control response measurements; screenshots. It keeps the screen awake temporarily and inhibits Waydroid freezing with a live user-process marker, then restores idle policy. Run through the mandated phone-control helper, using a transient system service so disconnection does not terminate the test.

`freeze/fail-gl.c` is a one-shot LD_PRELOAD fault injector; `freeze/test-shell.py` temporarily applies it to the real shell only when DemoLoop is inactive. `freeze/test-coredump.py` deliberately aborts a test process. These are test tools, not boot-installed components.

To undo deployment on the phone:

1. Wait until `systemctl --user is-active ztos-demoloop` is inactive.
2. Disable `ztos-memory-policy.timer` and `ztos-memory-policy.service`.
3. Restore files from `/var/lib/ztos-freeze-backup` to their original absolute paths. A `.absent` marker means the corresponding installed file did not previously exist and should be removed. Reload systemd afterward.
4. Restore `shmem_enabled` to `never` and Waydroid `memory.high` to `max`, the observed original values. Stop the browser before any attempt to `swapoff /dev/zram0`; leave swap enabled if memory cannot safely absorb its pages. A reboot clears runtime swap/THP/cgroup settings after the policy units are disabled.
5. Restart the shell only while DemoLoop is inactive. Its original binary is in the backup directory.

Raw diagnostic snapshots and verification artifacts are under `freeze/evidence/`; full phone-side artifacts remain in `/home/zerone/freeze-evidence`.
