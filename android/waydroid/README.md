# Waydroid Phase 1 on ztOS

This directory installs the Waydroid base on the Galaxy S9 / Exynos 9810 running
Arch Linux ARM, kernel 7.1, Waydroid 1.6.3 and the 20260927 Android 13 VANILLA /
MAINLINE images. Work was deployed over USB on 2026-10-23 UTC (2026-10-24 Colombo).
It does not install, remove or modify ATL or ztMETRO. Existing Android user data
at `/home/zerone/.local/share/waydroid` is retained.

Boot, IPv4 internet, 64-bit install/launch, lazy start, stuck-session recovery and
idle freeze were exercised. Long-run crash-free startup remains unproven; see the
validation limits below. The final tracing-disabled session restart had an empty
crash buffer and opened WhatsApp successfully.

## Install and launch

Copy this directory to the phone, then run as root:

```sh
sudo ./install.sh
```

The default builds the system image from `system.img.stock`, applies the pinned
vendor fixes, merges `waydroid.cfg`, regenerates Waydroid properties with
`waydroid upgrade -o`, and installs the services and launcher. Dependencies are
provided by the existing Waydroid installation, nftables, util-linux, e2fsprogs,
Python, sudo, and systemd. The target user is `zerone`, UID 1000.

To update configuration and hooks after the stripped image has been built:

```sh
sudo ./install.sh --config-only
```

Run as `zerone`, without sudo:

```sh
XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 \
  ztos-waydroid-launch com.whatsapp
```

`waydroid-container.service` is enabled and restarts on failure. The global
`ztos-waydroid-boot.service` starts the Android user session at desktop login,
waits up to 45 seconds for Android readiness, writes a boot marker, then freezes
Android under the normal idle policy. If Wayland or Android startup fails, it logs
the failure and leaves the lazy launch path available. ztMETRO keeps its splash
until that marker appears or its own 45-second timeout expires. The
`ztos-waydroid-session.service` itself has no Install section; the boot service and
launcher start it. The launcher checks D-Bus responsiveness, waits up to 90 seconds
for Android boot/package/platform readiness, then launches the app. Under ztMETRO it requires acknowledgement that the requested
package window is visible; outside ztMETRO it requires a nonzero native window count.
If the session is stuck or failed, it stops the old session, restarts the container
manager, and retries once. Concurrent launches share a user lock. A sudoers rule
allows only the three fixed recovery-helper actions, without arbitrary commands.

Phase 2 should call `ztos-waydroid-launch PACKAGE`. Direct `waydroid app launch`
bypasses the additional recovery logic. No shell integration was added here.

## Images and graphics

`strip-image.sh SOURCE OUTPUT` works on an ext4 copy, grows it temporarily because
the stock filesystem has zero free blocks, then removes the 26 requested app
directories. It runs `e2fsck`, `resize2fs -M`, adds 64 MiB slack and checks again.
The output must not already exist. The source image is never mounted writable.

The script also creates `system/etc/init/hw/init.zygote64.rc` from the primary
service in `init.zygote64_32.rc`. The supplied image has no `init.zygote64.rc`;
setting `ro.zygote=zygote64` alone leaves Android without Java services.
The config advertises only `arm64-v8a` and an empty 32-bit application ABI list.
32-bit vendor native services and their libraries remain: they do not require a
32-bit Java zygote. Applications containing only 32-bit native code cannot run.

HOME was resolved before stripping:
`com.android.launcher3/.uioverrides.QuickstepLauncher`. Launcher3 is retained.
`MmsService` is deliberately retained because of telephony dependencies. All other
listed removal directories were present and removed. Frameworks, permission XML,
WebView, LatinIME, DocumentsUI, ExternalStorageProvider, PackageInstaller,
Settings/SettingsProvider, Shell, NetworkStack and download/install providers remain.
There were no Google apps to remove from this VANILLA image.

Panfrost/GLES remains enabled; Vulkan remains disabled. Bootanimation is disabled
because its texture allocations failed under CMA pressure. Host output scale is 4:
360x672 logical configuration produces 1440x2688 Android pixels, with density 560.
A smaller `wm size` was investigated but not retained. No host display settings
were changed. The black background subsurface is disabled.

The September vendor also has two upstream composer defects. `patch-vendor.sh`
works on a copy and preserves `vendor.img.stock`. `patch-composer.py` validates the
entire original binary SHA256 before changing three AArch64 call instructions:

- Remove the redundant Binder threadpool reconfiguration, matching upstream
  [4aee4c8](https://github.com/waydroid/android_hardware_waydroid/commit/4aee4c8d953e5cf7cb58d838242b71899a51269a).
- Suppress configure-time display reconnects for this fixed 360x672 logical size,
  the fixed-size case of upstream
  [5af0c11](https://github.com/waydroid/android_hardware_waydroid/commit/5af0c114a19fc916de837ad553524cd2aade2719).
  Initial display registration remains intact. Resolution changes require a session
  restart; this deployment does not support dynamic resizing. Replace this workaround
  with a vendor rebuilt from those upstream fixes before adding dynamic resizing.

The script refuses an unknown binary or nonmatching geometry. The patched composer
SHA256 is `b9ab46a88b840da337aa6bbd37f309ae73463fd0d590f21636998f7530cd41ff`.
Applying the patch twice is safe; rejection of an unknown binary was tested.

## Networking

Upstream `/usr/lib/waydroid/data/scripts/waydroid-net.sh` already provides the
`waydroid0` bridge, dnsmasq DHCP/DNS, forwarding and nftables masquerade. It is
retained without package-file modifications. Android receives DNS 192.168.240.1;
dnsmasq forwards to the host resolver. No hard-coded external DNS bypass is needed.

The actual route failure is in the kernel: Android `ip rule` returns
`Operation not supported on transport endpoint`, and netd cannot initialize
RouteController. IPv6 multiple routing tables are also disabled. DHCP advertises
a default gateway and ConnectivityService shows it, but there is no kernel route.

`ztos-waydroid-network.service` runs `network.py` every five seconds. It enables
IPv4 forwarding and repairs its own `ip ztos_waydroid` NAT table if absent. It
uses the running container's network namespace to add a main-table default route
via 192.168.240.1 **only when IPv4 policy routing is unavailable**. Once a kernel
with working policy routing is installed, Android netd keeps route ownership.
It never flushes the host firewall or alters the host default route. This deployment
uses the existing 192.168.240.0/24 subnet and nftables firewall configuration.

After Android boot, the service sets `restricted_networking_mode=0`, sets the
Android screen timeout to 15000 ms and reconciles configured persistent runtime
properties. This matters because old properties in retained Android data override
image defaults. The service skips Android queries and route work when frozen.

The fallback restores IPv4 internet access; it does not implement Android VPN,
per-app policy routing, missing kernel netfilter/BPF features, or full IPv6 support.
Those require a kernel configuration update outside this phase.

## Idle freeze

`persist.waydroid.suspend=true` and `suspend_action=freeze` retain native Waydroid
suspend. Upstream documents the
[display-timeout trigger](https://docs.waydro.id/usage/waydroid-prop-options).
Testing found a case where Android remained awake after the last window closed,
with no wake locks, despite the 15-second display timeout.

The host service therefore adds a deterministic fallback. Once boot is complete,
it polls the composer's `waydroid.open_windows`. If the count stays exactly zero
and suspend remains enabled for 15 seconds, it calls `waydroid container freeze`.
With five-second polling, allow approximately 15–20 seconds. It counts all native
Android windows, not just the focused activity. An unfocused open Android window
keeps the container running. Missing/invalid counts never trigger a freeze.

The frozen cgroup is not polled from inside Android. The next launcher invocation
uses Waydroid's unfreeze operation and app launch to resume it. Android notifications,
background sync and calls cannot execute while the container is frozen; this is
the requested no-window policy.

Verified sequence: zero windows at 20:44:56 UTC, host logged freeze at 20:45:12,
`cgroup.freeze=1` at 20:45:16 and 20:45:26. CPU `usage_usec` stayed 236121741 across
those last two samples. A previous frozen interval also had zero CPU delta for
25 seconds. Relaunch restored RUNNING and an Android window count of 1.

The memory policy caps Android's cached process count at 16. Instagram and its
installed Meta push packages receive background AppOps `ignore`, the restricted
standby bucket and a boot-time force-stop. Packages listed in `keep_awake` are
excluded; foreground launches remain available. The cached-process and AppOps
settings persist in Android user data, so rollback must restore them explicitly.

## Measurements

Memory is `lxc.payload.waydroid/memory.current`, **not** the small systemd manager
cgroup. Values include file cache, shared graphics buffers and kernel charges.
Snapshots have different cache histories and workloads; they are not a controlled
benchmark or proof that every change independently saves RAM. No cache dropping
or artificial memory cap was used. `memory.max=max`, with zero cgroup OOM kills.
Host RAM is 3516 MiB, with no swap; ztMETRO was running throughout.

| Stage | Payload bytes | MiB | Host used / available MiB |
|---|---:|---:|---:|
| Original, warm phone, frozen; crashes present | 1,176,170,496 | 1121.7 | 3165 / 350 |
| Stripped + 64-bit init + no bootanimation, first cold boot, idle | 2,445,312,000 | 2332.0 | 2052 / 1464 |
| ART/LMKD tuned, before composer fixes; unstable graphics | 2,182,012,928 | 2080.9 | 1888 / 1627 |
| Composer fixes, idle and frozen | 1,536,876,544 | 1465.7 | 1800 / 1716 |
| Composer fixes, WhatsApp open | 1,744,039,936 | 1663.2 | 1914 / 1602 |
| Final config, tracing off, WhatsApp open | 1,774,587,904 | 1692.4 | 1776 / 1739 |
| Final config, tracing off, idle/frozen | 1,811,804,160 | 1727.9 | 1756 / 1760 |

Raw snapshots and the complete strip log are in [measurements/](measurements/).
The two composer-fix snapshots preceded disabling optional Perfetto collection.
Idle anonymous memory was 558,145,536 bytes; WhatsApp-open anonymous memory was
624,717,824 bytes. Much of the total is cache/graphics, not private application RAM.
The earlier ~750 MB idle figure was **not reproduced**. In the final frozen sample,
anonymous memory was 634,368,000 bytes and file memory was 1,089,392,640 bytes.
Cache ownership and sampling time explain why the final frozen total exceeds the
preceding WhatsApp-open total; host used memory fell from 1776 to 1756 MiB.

ART keeps the original 8m start, 128m growth limit and 256m maximum heap. Target
utilization changes from 0.6 to 0.75, minimum free from 8m to 2m and maximum free
from 16m to 8m. Low-RAM mode stays enabled. LMKD uses PSI without legacy minfree
threshold mode. Optional `traced` and `traced_probes` are disabled through the
standard `persist.traced.enable=0` property. Essential Android services remain. While WhatsApp was still loading, disabling
tracing changed memory.current from 2,365,927,424 to 2,430,214,144 bytes after three
seconds. That transient measurement does not demonstrate a memory saving.
Both tracing services were verified `stopped` on the following Android boot.

| Image | Stock bytes | Deployed bytes |
|---|---:|---:|
| system.img | 1,994,985,472 | 1,950,093,312 |
| vendor.img | 421,634,048 | 421,634,048 |

System image reduction is 44,892,160 bytes (42.8125 MiB, about 2.25%) after retaining
64 MiB slack. Removing apps does not eliminate framework/native library space.
Backups consume additional disk space and are intentionally retained.

## Validation and remaining limits

Three phone reboots returned with both host services active and the Android user
session stopped. One measured cold launch reached boot completion and returned
success in 35 seconds. Repeated container/session restarts retained HTTP 204.
Android reported default network 100, DNS resolution worked for the HTTP request,
and restricted networking was 0. Deleting the Android default route and only our
NAT table was repaired within seven seconds, followed by HTTP 204.

A newly signed test APK installed successfully after stripping. Its activity
reported `is64Bit=true`; a later cold launch completed in 1455 ms after composer
fixes. WhatsApp installed here already uses arm64-v8a. It launches to its registration
screen; account login, messaging, calls and recovery of historical chats were not
validated. Existing WhatsApp data was not reset or reinstalled.

Stopping the session manager with SIGSTOP tested a stuck session. The next launcher
invocation timed out its D-Bus check, killed the stuck service, restarted Android,
and reopened WhatsApp. The session manager PID changed from 6656 to 10675.

**Strict crash-free startup is not yet proven.** The composer Binder aborts and
configure-time display reconnect failures stopped in subsequent tests. A later
cold boot still recorded one SurfaceFlinger SIGSEGV in Perfetto's TracingMuxer /
`UnixTaskRunner::AddFileDescriptorWatch`, followed by a Settings window error.
Android recovered and reached boot completion; subsequent app launches worked.
Perfetto collection was then disabled. The final Android session restart reached
boot completion, showed one native WhatsApp window, stopped both tracing services,
and had an empty crash buffer. This does not establish the earlier crash's cause
or prove cold-phone startup and long-run graphics stability. That remains the
Phase 1 stability limitation; total memory also remains substantial, and the
kernel policy-network features are still missing.

## Reproduce checks

Keep the phone screen awake during interactive testing:

```sh
printf 'screen on\n' > /run/user/1000/ztmetro.ctl
```

As root, while Android is running:

```sh
./measure.sh
waydroid shell -- cmd package resolve-activity --brief \
  -a android.intent.action.MAIN -c android.intent.category.HOME
waydroid shell -- getprop waydroid.open_windows
waydroid shell -- dumpsys SurfaceFlinger | grep 'GLES:'
waydroid shell -- settings get global restricted_networking_mode
waydroid shell -- curl -sS -o /dev/null -w '%{http_code}\n' \
  --max-time 8 http://connectivitycheck.gstatic.com/generate_204
```

`measure.sh` avoids attaching to a frozen container. To measure a frozen interval,
use only host reads; `waydroid shell` may thaw it:

```sh
lxc-info -P /var/lib/waydroid/lxc -n waydroid -sH
cat /sys/fs/cgroup/lxc.payload.waydroid/{cgroup.freeze,cpu.stat,memory.current}
free -m
journalctl -u ztos-waydroid-network.service --no-pager -n 15
```

Close every Android window, wait 20 seconds, verify FROZEN, then launch an app and
verify RUNNING. The final freeze test showed HWC count 0 at 20:50:25 UTC,
a host freeze log at 20:50:42, and FROZEN at 20:50:45 and 20:50:55. CPU usage
stayed exactly 199797248 microseconds across those frozen samples. The test APK
was uninstalled after verification; WhatsApp was only stopped, never removed.

Test installation with a trusted APK containing arm64 native code
or no native code: `waydroid app install /path/app.apk`, then use the lazy launcher.

## Rollback

The original config is `/var/lib/waydroid/ztos-backup/waydroid.cfg`. Original images
are `/usr/share/waydroid-extra/images/{system.img.stock,vendor.img.stock}`.
Stop the user session and container manager before replacing either image. Restore
**both stock images and the original config together**, run `waydroid upgrade -o`,
and restart the manager. Reverting only the system image while leaving zygote64
configured will fail because the stock image lacks its init file.

Disable/remove only the `ztos-waydroid-*` services, including the global user
boot unit and its `/usr/local/libexec/ztos-waydroid-boot` helper, installed launcher,
`/etc/sudoers.d/ztos-waydroid` and container `ztos-recovery.conf` drop-in to remove
host changes. Delete only the owned `ip ztos_waydroid` nftables table if desired.
Retained Android settings such as screen timeout, restricted networking and
`persist.*` properties are independent of image rollback; restore those explicitly
if desired. The boot memory settings also persist in Android user data. To restore
the default cached-process limit and background policy after starting Android:

```sh
waydroid shell -- cmd device_config delete activity_manager max_cached_processes
for package in com.instagram.android com.facebook.services com.facebook.appmanager; do
  waydroid shell -- appops set "$package" RUN_IN_BACKGROUND default
  waydroid shell -- appops set "$package" RUN_ANY_IN_BACKGROUND default
  waydroid shell -- am set-standby-bucket "$package" active
done
```

Disable the global boot unit with `systemctl --global disable ztos-waydroid-boot.service`,
then reload the user manager. Remove its unit and helper if desired. Never remove
`/home/zerone/.local/share/waydroid` or ATL data.

## ztOS runtime choice

Phase 2 adds `ztos-android install APK [--runtime atl|waydroid]`, `launch PACKAGE`,
`uninstall PACKAGE`, and `list`. With no explicit runtime, the CLI reads
`~/.config/ztos/android.conf`; use `default_runtime=atl` or
`default_runtime=waydroid`. Without a valid setting, it asks on a TTY and uses ATL
otherwise. Each managed app has one desktop entry and a launcher icon extracted from
its APK. The runtime appears in the entry comment. The ztMETRO scanner ignores
Waydroid-generated `waydroid.*.desktop` entries, which Waydroid can recreate.

Waydroid windows use the `waydroid.PACKAGE` app ID. ztMETRO treats them as Android
windows, maps them to their app tile, and closes their Android task from recents.
The shell sends Escape for Back. zrnperfd reads the Waydroid payload cgroup's CPU
usage while one of those windows is focused and confines the container to the
efficiency-core cluster; the measured workload still controls dynamic boost.

The compositor freezes Waydroid before screen-off SoC sleep and when no Waydroid
window is shown. It keeps the container running for an installed package listed in
`keep_awake`, a comma-separated list in the same config file. For example:

```ini
default_runtime=atl
keep_awake=com.whatsapp,org.telegram.messenger
```

Only registered Waydroid packages in that list prevent freezing. Frozen apps cannot
run background notifications, sync, or calls. A launch-in-progress marker prevents
the event-driven policy and Phase 1's zero-window fallback from freezing Android
while its first session or APK installation is starting.

Opening an APK in ztMETRO Files offers ATL, Waydroid, and both remember-as-default
choices. Updating the default preserves the other `android.conf` entries.

Device results and outstanding validation are in [PHASE2.md](PHASE2.md).

The device exposes Android's IME as a separate `waydroid.InputMethod` toplevel,
without text-input-v3. ztMETRO uses that surface's map/unmap signals to show its
own keyboard while keeping focus on the Android app. It excludes the IME from
recents. Host key events reach Android; Back hides both the host keyboard and
Android's IME. Closing a Waydroid recents card force-stops the package without
terminating the shared Waydroid compositor process.
