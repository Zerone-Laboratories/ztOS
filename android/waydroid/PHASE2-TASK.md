# Waydroid runtime for ztOS — Phase 2: ztMETRO integration + runtime choice

Read first: android/waydroid/README.md (Phase 1: lean 64-bit LineageOS 20 image, GPU via
Mesa/panfrost, network service, lazy session start, zero-window freeze, recovery). Phone
access as in PHASE1-TASK.md (`python /tmp/ztos-phone-control.py run|sudo|put|get`).
Graphical env: `XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0`; control socket
`/run/user/1000/ztmetro.ctl` accepts `screen on`, `tap X Y`, `down ID X Y`, `move ID X Y`,
`up ID` (logical 360x740 coordinates). Screenshots: `grim` with that env.

ztOS offers two Android runtimes; the user picks per app at install time:
- `atl`  = ztOSAndroidRuntimeV1 (Android Translation Layer, process
  `android-translation-layer`, launched by `ztos-android launch PKG`),
- `waydroid` = this container.

Constraints (hard):
- The phone is often unattended. ztMETRO must never be left broken: keep the previous
  binary as a backup, verify the shell restarts cleanly (greetd respawns it on pkill), and
  roll back on failure.
- All UI must stay GPU-drawn; tune for smooth use on the four efficiency cores alone.
- Do not uninstall or break the ATL runtime; existing ATL-installed apps keep working.
- Do not delete Android user data.

Deliver:
1. `ztos-android` CLI (repo: os/overlay/usr/local/bin/ztos-android, deployed to
   /usr/local/bin/ztos-android): `install APK [--runtime atl|waydroid]` (default from
   `~/.config/ztos/android.conf` key `default_runtime`, falling back to asking on a TTY,
   else atl), `launch PKG` dispatching on the app's recorded runtime, `uninstall PKG`,
   `list` (shows runtime). Record the runtime in the existing app registry
   (applications.json the CLI already maintains). Waydroid installs use the Phase 1
   launcher/recovery (lazy session start, unfreeze, `waydroid app install/launch`).
2. App list: one ztOS-managed `.desktop` per installed app regardless of runtime, label
   "<App name>" with the runtime shown subtly (e.g. Comment or a suffix the shell can
   render), correct icon (extract from the APK), Exec=`ztos-android launch PKG`.
   Waydroid's auto-generated `waydroid.*.desktop` entries (system apps like Settings,
   Files, Contacts) must not appear in ztMETRO's app list (NoDisplay, or ignore them in
   the scanner — pick the robust option and make it survive Waydroid regenerating them).
3. ztMETRO (ztmetro/ztmetro.c, C, wlroots; build with the repo Makefile/toolchain used
   before — find how ztmetro is built and deployed for aarch64):
   - Treat Waydroid windows (app_id `waydroid.<package>` in multi-window mode — verify
     the real app_id on the device) as Android-runtime windows: map them to their app
     tile/recents entry, set the existing `android_runtime` flag (ztmetro.c ~6849 only
     matches ATL now) so keyboard resize behaviour applies.
   - Back button already sends KEY_ESC to the shown app; verify Android maps it to BACK
     in Waydroid windows; if not, fix in the least invasive place.
   - Keyboard: verify whether Waydroid windows use ztMETRO's on-screen keyboard
     (text-input-v3) or Android's own IME. Make exactly one keyboard appear, prefer the
     ztMETRO keyboard if Waydroid supports text-input; report what you chose and why.
   - Closing an app from recents closes the Android task (not the whole container).
4. Performance: zrnperfd (ztmetro/perfd/zrnperfd.c) dynamic boost must see Waydroid's
   CPU work (the container's cgroup) when a Waydroid window is focused; keep idle cores
   low otherwise.
5. Battery: freeze the container when the screen turns off and when no Waydroid window is
   shown (Phase 1 has a zero-window fallback — make it event-driven from ztMETRO if
   cheap); unfreeze instantly on launch. Coordinate with zrn-sleepd (SoC sleep when screen
   off) so freeze happens before SoC sleep. Note in the README that frozen apps get no
   background notifications, and add a per-app opt-out list (`keep_awake` in
   android.conf) that keeps the container running for messaging apps.
6. Files app (ztmetro/files): opening an `.apk` offers "Install with ztOS runtime" and
   "Install with Waydroid" (plus remember-as-default).
7. Verify on the device with screenshots: install a small APK with each runtime, both
   appear in the app list with icons, launch both, back works, keyboard appears once,
   recents close works, container freezes after leaving and on screen off, boost engages.
Write `android/waydroid/PHASE2.md` with results and anything left undone.
