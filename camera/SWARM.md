# Camera swarm report — 2026-10-25

## Result

Driver research is complete to the safe read-only boundary. The Lumia-style
camera app builds on the S9, is installed, and passes phone regression and UI
checks with an explicitly labelled test scene and YUYV replay. **Hardware camera
capture is not working:** the running kernel disables MEDIA_SUPPORT and exposes
no video or media devices. No kernel, module, mount, clock, cpuidle, Waydroid or
shell restart was performed. No reboot, flash, commit or push was performed.

The previous report's connection block is resolved. Historical failures remain
in `evidence/phone-access.txt`; fresh phone evidence supersedes them.

## Files and changes

* `camera/CAMERA-DRIVERS.md`: live kernel/config and stock vendor inventory,
  local source locations, FIMC-IS2/sensor/CSIS/PAF dependencies, firmware hashes,
  downstream module feasibility, mainline/libcamera limitations, staged plan,
  effort and risks. Stock vendor is actually `/mnt/vendor`, mounted read-only;
  `/mnt/stockvendor` is not mounted. Matching 2L3/3H1 setfiles and ISP libraries
  exist. Live kernel is `7.1.0-zboot`; libcamera is not installed.
* `ztmetro/camera/ztcamera.c`: GPU-composited portrait viewfinder, right-side
  photo/video cluster, Segoe UI Symbol U+E114 camera shutter, five draggable
  manual arcs, roll thumbnail, source menu, front test card, timer and grid.
  Sources are single-plane V4L2 YUYV, packed YUYV replay and animated fallback.
  Continued existing app work and preserved its comments/style. Corrected zero
  EV default, queue-before-stream order, partial V4L2 cleanup, bounded buffers,
  padded YUYV stride, short/error frames, below-black luma, replay dimensions,
  capture filename collisions, recording write failures and source-size changes
  while recording. Fixed frozen scene with open dial and slow-shutter trails.
  Phone testing found every button rejected release as a drag: Core sends
  touch-up with `(0, 0)`. Camera now retains the last down/motion coordinates.
  Regression tests cover tap activation and drag cancellation. Moved arc center
  right so labels remain on screen. Enlarged integer formatting buffers.
* `ztmetro/camera/test-camera.c`, `check.sh`: source/capture/input regressions
  and six offline reference renders, executed successfully on the phone.
* `ztmetro/camera/README.md`, `.desktop`, `.svg`: usage, build/install, source
  semantics, known limitations, references and desktop integration.
* `camera/evidence/`: actual phone screenshots, build/test/deployment logs,
  live inventory and firmware hashes. UI verification scripts are retained here
  for reproducibility; they require the phone's running compositor.

Only camera directories were edited. The existing shared Makefile already
provides `make camera/ztcamera` and install rules; it was not edited here.
Current Core sources were copied into the private phone build tree, not edited.
No kernel patch was justified: safely enabling this hardware is a larger port.

## Phone build and deployment

Used only `/tmp/ztos-phone-control.py` for phone access. Reused the existing
private `~/swarm-camera` copy, updated its camera/Core build inputs from this
repository, and ran `sh camera/check.sh`. No shared phase2 build ran.

Installed `/usr/local/bin/ztcamera`, desktop entry and scalable icon. There was
no previously installed camera binary to back up; no backup was overwritten.
Installed executable SHA-256 matches the private build:
`9e75b0d120008bcbf37bc20232217a3f4028a6df73c2483bb28b7dfad5e2fcca`.
`desktop-file-validate` passed. `launch ztcamera` through the shell control FIFO
started the installed app after termination; `launch Camera` is not valid because
that verb expects an executable command. Installed artifacts are persistent;
reboot was deliberately not tested. No new startup service is needed.

Saved user config before tap tests and restored it byte-for-byte afterwards;
`cmp` passed. Closed the sensor framework dialog through `dialog_close`, never
by tapping its buttons. No shell restart was needed.

## Verification and screenshots

Phone regression harness passed replay bounds/truncation, padded YUYV and black
levels, repeated capture names, recording write failure, coordinate-free release
and drag cancellation. See `evidence/camera-check.log`. Build warnings remain in
shared Core for missing `zmc_hit.button` initializers; camera warnings found in
this run were fixed. `sh -n camera/check.sh` and task-path `git diff --check`
passed locally. No local executable build was performed.

Pulled and visually inspected actual grim screenshots:

* `evidence/camera-desktop-launch.png`: installed app launched by shell.
* `evidence/camera-final-dial-0.png` through `camera-final-dial-4.png`: WB,
  focus, ISO, shutter and exposure arcs, including corrected unclipped labels.
* `evidence/camera-final-drag.png`: ISO changed to 100 by a real control gesture.
* `evidence/camera-photo.png`, `camera-roll.png`: repeated shots and latest-image
  review. Files `IMG_261025_074159_0.png` and `_1.png` are 14317/14290 bytes.
* `evidence/camera-front.png`: explicitly labelled front test card.
* `evidence/camera-recording.png`, `camera-recorded.png`: video starts and stops.
  `VID_261025_074206.bgra` is 40550400 bytes, exactly 33 complete 640x480 BGRA
  frames. These generated samples remain on the phone; user data was not deleted.
* `evidence/camera-replay.png`: real replay of a generated two-tone YUYV fixture.
* `evidence/camera-invalid-device.png`, `camera-invalid-replay.png`: explicit
  missing-source notices with working fallback scene.
* `evidence/camera-efficiency.png`: app and renderer threads restricted to CPU
  0–3, showing 23 fps; affinity output is `efficiency-check.txt`. No frequency
  or global CPU policy changed. The scene generally showed 21–27 fps; this is
  not proof of 60 fps animation or exhaustive performance testing.

The reference AAWP Lumia 1020 article was consulted: circular controls over the
viewfinder, manual settings and shutter cluster are adapted to portrait layout.
The result is an adaptation, not a pixel-identical Lumia screenshot.

## Remaining limitations and next work

* No working hardware camera or live V4L2 capture test is claimed. Kernel media
  support, sensor power/clock/reset, CSIS/D-PHY/DMA and media graph are missing.
  Downstream FIMC-IS2 is bool-based 4.9 code with Samsung ION/IOMMU and binary ABI
  dependencies; it cannot safely be inserted into this 7.1 kernel as-is.
* Current postmarketOS starlte wiki remains inaccessible through its redirected
  anti-bot host. Local mainline/S9+ evidence and libcamera documentation do not
  establish Exynos9810 camera support. The research doc distinguishes this
  uncertainty from verified live kernel facts.
* GLES handles frame/UI composition; CPU generates scene pixels and converts
  YUYV. Hardware supports only single-plane YUYV in this app. Hardware ISO,
  shutter, focus/autofocus and front/rear selection are not wired. Replay/real
  sources apply EV/WB in software; all five dials affect the test fixture.
* Video is raw BGRA without audio, timestamps or container, capped at 48 MiB;
  recording cadence varies. Roll opens the latest PNG rather than a full gallery.
  Viewfinder fills app content, while shell status/navigation bars remain visible.
* Reboot and sensor/ISP experiments were outside tonight's safe scope. Next
  hardware work needs attended recovery: inspect supply/clock ownership, read
  sensor ID, then port one raw sensor/receiver mode before considering libcamera
  or Samsung firmware ABI reuse. Estimates are weeks–months for first raw frames,
  months or longer for tuned photography.
