# Waydroid runtime for ztOS — Phase 1: lean, fast, stable base

ztOS (Arch Linux ARM on a Samsung Galaxy S9 / Exynos 9810, Mali-G72 panfrost, 3.5 GB RAM,
kernel 7.1 with binder/binderfs) offers two Android runtimes: ztOSAndroidRuntimeV1 (ATL)
and Waydroid. This phase makes Waydroid lean and stable. ztMETRO (the shell) integration
is Phase 2; do not touch ztmetro/ or the ATL runtime.

Phone access (USB network): `python /tmp/ztos-phone-control.py run '<cmd>'` (user zerone),
`python /tmp/ztos-phone-control.py sudo '<cmd>'` (root), `... put <local> <remote>`,
`... get <remote> <local>`. Graphical env for the user session:
`XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0`. Keep the screen awake while
testing with `printf "screen on\n" > /run/user/1000/ztmetro.ctl`.

Current state (verified):
- waydroid 1.6.3 (Arch package), images /usr/share/waydroid-extra/images (LineageOS 20,
  Android 13, VANILLA, MAINLINE vendor), config /var/lib/waydroid/waydroid.cfg.
- GPU works: SurfaceFlinger reports `Mesa, Mali-G72 MC18 (Panfrost), OpenGL ES 3.1`
  (props ro.hardware.gralloc=gbm, ro.hardware.egl=mesa, vulkan disabled — an earlier
  attempt crashed in vulkan.pastel.so with SIGILL; keep Vulkan off).
- multi-window mode on, ro.config.low_ram=true, lcd density 560.
- Problems: 32-bit zygote FatalError at boot; Android has no default route (no
  internet — earlier attempt also had "Restricted networking" on); container ~750 MB
  right after boot; sys.boot_completed not confirmed.
- User data in ~/.local/share/waydroid (WhatsApp data from an earlier install) — keep it.

Goals (measure before/after each change, report numbers):
1. Boot reliably to sys.boot_completed=1 with no crash loops. Make the container
   64-bit only (zygote64 only: e.g. ro.zygote=zygote64 via [properties], and
   ro.product.cpu.abilist32 empty if needed) — verify apps still install/run.
2. Network that survives container/session restarts and phone reboots: default route,
   DNS, NAT on the host (waydroid-net.sh, nftables/iptables as available), Android
   "restricted networking" off, verify `curl`-like check inside Android (e.g. `ping`,
   `dumpsys connectivity` default network, an HTTP 204 from connectivitycheck).
3. Small footprint: build a stripped system image without Google/LineageOS apps the
   user does not need. Work on a copy (`/usr/share/waydroid-extra/images/system.img.stock`
   backup kept). Remove from the image: product/app {Aperture, ApertureLensLauncher,
   Backgrounds, DeskClock, Etar, ExactCalculator, Gallery2, Jelly, LocalContactsBackup,
   PhotoTable, Recorder}, product/priv-app {Eleven, Contacts}, system/app {BasicDreams,
   EasterEgg, PrintRecommendationService, NfcNci, SecureElement, WallpaperBackup,
   HTMLViewer, CameraExtensionsProxy}, system/priv-app {LiveWallpapersPicker,
   ManagedProvisioning, BuiltInPrintService, MmsService?, DynamicSystemInstallationService,
   MtpService}. KEEP anything the framework, permissions, installs, WebView, IME
   (LatinIME), DocumentsUI/ExternalStorageProvider (file picking), PackageInstaller,
   Settings/SettingsProvider, Shell, NetworkStack, DownloadProvider, and a HOME activity
   needs (check which package provides HOME in Waydroid multi-window mode before removing
   any launcher-ish app). Shrink the ext4 (e2fsck -f; resize2fs -M; keep a little slack).
   Verify boot and app install after stripping. Prefer packaging the removal as a
   reproducible script in the repo over manual edits.
4. Memory: tune for 3.5 GB RAM with the ztOS shell running: low_ram, lmkd PSI, dalvik
   heap sizes, disable unneeded Android services if safe (e.g. via `pm disable-user` for
   packages that must stay in the image). Report container memory (cgroup
   memory.current) idle after boot and with WhatsApp open, and host `free -m`.
   No hard cgroup cap below what Android needs (earlier 1 GiB cap caused OOM kills).
5. Battery: container must be frozen when no Android window is in use. Waydroid has
   `suspend_action = freeze` and `persist.waydroid.suspend`; verify it freezes when no
   Android window is shown and unfreezes on launch. Document the exact trigger.
6. Recovery: the phone is often unattended — every boot must recover by itself.
   waydroid-container.service enabled; the user session should not start until an Android
   app is launched (lazy start). A stuck/failed session must self-heal on the next launch.

Deliverables in the repo under `android/waydroid/` (create files there, executable bits
where needed): image strip script, props/config file the installer applies
(waydroid.cfg [properties] block), host network setup (systemd unit or hook), a
`README.md` with the measured numbers and how to reproduce, and deploy the result on
the phone. Do not delete user data. Do not uninstall the ATL runtime. Report concrete
numbers and anything you could not finish.
