# Shell freezes while browsing in Superscan (GPU buffer allocation failures + coredump)

Phone access ONLY via `python /tmp/ztos-phone-control.py run|sudo|put|get` (never plain ssh;
two failed logins = stop). Build on the phone in /home/zerone/ztmetro-phase2 (push only the
files you change; local ztmetro.c/perfd contain other agents' in-progress work that is already
mirrored there). Kernel source: build/linux-7.1 (modules can be built and loaded without
flashing, see memory notes in the repo READMEs). Another agent (DemoLoop, ztmetro/DEMOLOOP-TASK.md)
may be running on the phone: don't restart ztMETRO while `systemctl --user is-active
ztos-demoloop` is active; coordinate by waiting.

## Evidence (2026-10-24 08:58 UTC, user browsing uptodown.com in Superscan)
- Kernel, repeated: `WARNING: drivers/gpu/drm/panfrost/panfrost_mmu.c:478 at
  panfrost_mmu_map+0xc0/0xcc, CPU#4: ztmetro/531` <- panfrost_gem_open <-
  panfrost_ioctl_create_bo. Line 478 is `WARN_ON(IS_ERR(sgt))` after
  drm_gem_shmem_get_pages_sgt(): shmem pages for a new GPU BO could not be allocated.
  Boot log: `panfrost 17500000.gpu: [drm] Using Transparent Hugepage`.
- superscan (pid 4859) dumped core in its GPU present path: present_gpu -> zmg_end
  (ztmetro/browser/superscan.c). systemd-coredump then used 9.5 s CPU, 357 MB peak over 15 s.
- No swap; MemAvailable ~1.4 GB after the event; Waydroid container may hold ~700 MB.
- User: the browser glitched, the whole shell froze, then recovered; "this is how the OS
  crashed other times too".

## Deliver
1. Root cause of the shmem page allocation failure: errno (add a temporary debug print or
   use ftrace/kprobe on drm_gem_shmem_get_pages_sgt return), memory state at that time
   (MemFree, fragmentation /proc/buddyinfo, shmem, THP), whether panfrost's huge-tmpfs
   (THP) mount is the cause (high-order allocation failing instead of falling back), who
   holds GPU memory (ztmetro, superscan, waydroid; /sys/kernel/debug/dri/*/gem or fdinfo).
   Reproduce: load heavy pages in Superscan (uptodown.com, image-heavy sites) while
   watching dmesg.
2. Fix at the right layer, smallest change first: e.g. panfrost THP off (module param /
   kernel cmdline alternative / sysfs), vm.min_free_kbytes / watermark_scale_factor for an
   aarch64 phone without swap, zram swap (small, lz4) so anon memory can be compressed,
   Superscan GPU memory limits (texture/tile cache caps, release on background), and a
   bounded Waydroid memory footprint. Never a hard cgroup cap that OOM-kills Android.
3. ztMETRO and Superscan must survive a failed BO allocation without freezing the shell:
   check their GL error handling on allocation failure (skip the frame, drop caches, retry)
   — the shell must keep drawing.
4. Coredumps must not freeze the phone: systemd-coredump config (e.g. ProcessSizeMax /
   ExternalSizeMax small, or Storage=none for user apps, with journal-only backtraces) and
   run it at low priority (nice/ioprio drop-in).
5. Verify: 10 minutes of heavy browsing in Superscan with Waydroid running, no panfrost
   warnings, no shell stalls >250 ms (ztmetro.log slow frames), memory numbers before/after.
Write ztmetro/FREEZE.md with root cause, numbers, and changes.
