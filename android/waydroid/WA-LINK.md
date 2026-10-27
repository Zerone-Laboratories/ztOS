# WhatsApp companion link: Waydroid

## Finding

The captured failure points to KeyMint device-ID attestation. On 2026-10-24, logcat
recorded `keystore2: In generate_key. 1: Error::Km(ErrorCode(-66))` during the
WhatsApp linked-device flow. KeyMint error -66 is `CANNOT_ATTEST_IDS`.

Package state and ActivityTaskManager logs identify WhatsApp as UID 10125. The
`LinkedUsersActivity` launch preceded the captured keystore error by 671 ms.
This ties the failure to WhatsApp's linking flow, though this logcat does not
expose the Binder caller PID/UID on the individual `generate_key` request.

Original Waydroid product IDs were brand `waydroid`, device `waydroid_arm64`,
manufacturer `Waydroid`, model `Samsung Galaxy S9 (SM-G960N)`, and name/product
`lineage_waydroid_arm64`. Matching `_for_attestation` properties were added in
`waydroid.cfg`. A challenge-plus-device-ID EC probe still got -66, so properties
alone did not solve it.

## KeyMint and DEX fixes

Software KeyMint in this Waydroid image cannot attest device identifiers. The
first framework fallback patched
`KeyGenParameterSpec.isDevicePropertiesAttestationIncluded()` to return false.
It suppressed device-ID attestation while retaining the challenge. The current
variant also makes `KeyGenParameterSpec.getAttestationChallenge()` return null,
to test whether WhatsApp accepts ordinary keys without an attestation
extension. This mirrors ATL's plain-key behavior. The patch is reproducible with
[`patch-id-attestation.py`](patch-id-attestation.py), applied with
[`apply-id-attestation.sh`](apply-id-attestation.sh), and reversible with
[`restore-id-attestation.sh`](restore-id-attestation.sh). The apply script keeps
an original framework backup and does not touch WhatsApp data.

Waydroid restarted after patching. The first repack preserved DEX compression,
which caused repeated mmap fallback warnings and made WhatsApp/Instagram launch
slowly. The patcher now stores each DEX entry uncompressed, aligns payloads to
four-byte boundaries, and preserves compression for every other JAR entry.
Runtime `framework.jar` SHA256 is
`fa6d1a28aa9cbfc2ae7c441fe695c84e0f4dbe9b004488063b1dcb42a60b8bbc`. All four
DEX entries were checked from the running Android image; each was stored and
four-byte aligned. No mmap fallback warning appeared in the post-fix capture.

The first boot after the DEX change ran `dex2oat64` for 29 seconds. Android
logged a profile checksum mismatch for `classes3.dex`. A later boot rebuilt for
59 seconds and hit one system_server Watchdog kill while Dex verification was
active. `odrefresh --force-compile` has now built persistent boot artifacts in
`/data/misc/apexdata/com.android.art/dalvik-cache/arm64` (`boot.art`, `boot.oat`,
and `boot.vdex`). `odrefresh --check` exited 0. The generated artifacts are
present in the live cache; a cold-restart reuse check remains pending. The
image's original read-only `/system/framework/arm64/boot-framework.*` files
remain untouched. Current startup still logs slow SystemUI and system_server
work, so overall responsiveness needs a post-boot check. No new framework DEX
mmap warnings appeared.

Missing `/dev/cpuset/top-app` and `/dev/stune/top-app`, and LMKD `MemSoftLimit`
lookup failures, also appear in logs. RAM-BOOT changed AppOps and standby policy;
it did not change cgroups. These messages match Waydroid's Android cgroup
expectations not being mounted in the host container, so no speculative cgroup
mount was added. Android also logs BPF network allowlist failures, but the
WhatsApp HTTP task setup succeeded during the retry window.

## Probe and retry state

The requested probe source is
[`KeyMintAttestationProbe.java`](tests/KeyMintAttestationProbe.java). It asks for
an EC key, a challenge, and device-property attestation, then checks the
certificate chain and attestation extension. Before patch, it returned -66.
After the first patch, the `app_process` invocation failed after entering Android
Keystore's remote provisioning path because `app_process` had no application
context. Variant A is deployed but has not yet passed an app-context probe. Its
DEX getter bodies were checked in the generated JAR; certificate behavior still
needs runtime verification.

The 11:12 retry capture contains no `ErrorCode(-66)` entry, matching the user's
report that the KeyMint error stopped after the framework patch.

At 11:12:26 UTC, WhatsApp PID 1807 (UID 10125) entered EULA then
`LinkedUsersActivity`. Android started Instagram PID 2141 (UID 10057) for
`FoaBackupTokenProvider`; it logged provider instantiation. Companion activity
opened at 11:12:37 and closed at 11:13:10, then WhatsApp returned to EULA. The
Instagram log had `CrashLoopRemedyLog` warnings (`instaCrash: false`, then
`true`), but no AndroidRuntime fatal exception for PID 2141. WhatsApp's WCIHTTP
task setup reported success; no explicit registration HTTP failure appeared.
WhatsApp also logged missing Play Store / `SERVICE_INVALID` Google Play services,
and an ancillary retriable private-stats signature error. Missing Play services
remains a plausible link blocker; the capture does not show a decisive server
registration response. Link still failed.

Instagram had `RUN_IN_BACKGROUND=ignore`, `RUN_ANY_IN_BACKGROUND=ignore`, and
standby bucket `45` during that attempt. The live AppOps now allow both background
operations, and standby bucket is active (`10`). `network.py` keeps this policy
across boot while retaining the cached-process limit. The provider started, so
no cross-app permission denial was logged.

Fresh host-side `waydroid logcat -- -v threadtime` capture is running as PID
118475 at `/var/tmp/wa-link/retry.raw`; it was restarted after Android restarted
and the old logcat process exited. `/var/tmp/wa-link/RETRY` exists with scan
instructions. The earlier capture remains at `/var/tmp/wa-link/post-patch.raw`.
`tcpdump` is unavailable. ART refresh is complete. Android is running and the
network service is active. Instagram policy reads AppOps `allow` and standby
bucket `10`. Instagram policy has not yet had a post-change link attempt.

## 11:27–12:07 retry and variant A

The 11:27–11:35 capture contains a fresh boot at 11:33. WhatsApp UID 10125
entered `LinkedUsersActivity` at 11:35:03. It repeatedly reported that the
Google Play Store was missing; Play services returned `SERVICE_INVALID`, and
Android could not bind Play Core's express-integrity service. WhatsApp created
WCIHTTP tasks successfully. The capture contains no explicit HTTP registration
response or server rejection. The user's “Download the official WhatsApp” dialog
therefore points toward missing Play Integrity/Play Store support, but does not
prove that this is the server's rejection reason.
No `jadx` executable or repo-bundled decompiler was available, so WhatsApp's
obfuscated decision branch was not decompiled.

Source APK is `/home/zerone/.local/share/ztos/android/apks/com.whatsapp-waydroid.apk`,
SHA256 `a6ef23a075243190255614f0ff71d1a39f935258a4eee9ef03e01823a3806748`.
Installed `/data/app/~~E10CeRCEMHPNO0_zLzJP_Q==/com.whatsapp-LpsRP0quGzIBS_D0rc9hYw==/base.apk`
has the same SHA256.
Its embedded signer certificate subject is `C=US, ST=California, L=Santa Clara,
O=WhatsApp Inc., OU=Engineering, CN=Brian Acton`; SHA256 fingerprint is
`39:87:D0:43:D1:0A:EF:AF:5A:87:10:B3:67:14:18:FE:57:E0:E1:9B:65:3C:9D:F8:25:58:FE:B5:FF:CE:5D:44`.
The signer identity matches WhatsApp Inc. metadata, but no external official
fingerprint reference was available for independent confirmation. The phone has
no separate ATL WhatsApp APK for direct comparison. Installer
source and installed PackageManager signer details remain unverified because
the shell attach path returned “needs root access” / “Insufficent privileges to
control waydroid.” The framework patch does not touch PackageManager signature
reporting.

Variant A is live. The patcher also replaces
`getAttestationChallenge()` with `return null`; resulting framework JAR SHA256
is `6f3b2d699407037939196ebc7d5b02bfb58eda23f2b627ca49218dc74e276a5a`. All four
DEX entries remain stored and four-byte aligned. Android restarted. Its boot
`dex2oat64` pass took 25.512 seconds after the framework change. The post-restart
capture has no framework mmap/zipalign warning and no KeyMint `-66` through
12:06:41. Android system_server restarted once during boot and came back.
WhatsApp opened EULA then `LinkedUsersActivity`. The network policy service is
active. Instagram's post-restart AppOps and standby readback remain unverified
because LXC shell attach is blocked. The 11:55 pre-restart readback showed
background AppOps `allow` and standby bucket `10`. No WhatsApp data was removed.

Fresh capture runs as PID 126809 at `/var/tmp/wa-link/retry.raw`.
`/var/tmp/wa-link/RETRY` requests one attempt against variant A. Results are
pending. The deployment script's Android readiness helper failed on this host's
LXC attach permission boundary; framework deployment and restart succeeded, then
the network service and WhatsApp launcher were started manually. Repair script
readiness before relying on it for future deployment.

## Reproduction

```sh
android/waydroid/tests/build_attestation_probe.sh android/waydroid/.wa-link-build
```

Deploy the resulting dex as `/data/local/tmp/KeyMintAttestationProbe.dex`, then
run it with `app_process`. Delete the probe alias on completion; it is scoped to
the dedicated probe alias and contains no WhatsApp data.

## 2026-10-24 microG deployment (supersedes variant A state above)

microG is installed in Waydroid. WhatsApp's welcome screen no longer displays
its custom-ROM warning. The welcome screen shows “Welcome to WhatsApp” and
“AGREE AND CONTINUE”; no phone number was entered and no message was sent.
Registration and companion-link acceptance still require the user's next test.

The live framework is stock again, SHA256
`84448fd7d7b793a53fb25b55c5decfeadb0736f29ab50fa0c6bd8eb0b4980cb7`.
Neither attestation getter is patched for this microG-first test. Build props
remain the previously configured Samsung user/release-keys identity. Instagram
remains disabled-user. Both ATL and Waydroid WhatsApp data were retained.

### Installed components and spoofing

Official, checksum-pinned releases are installed as privileged system apps:

- GmsCore `v0.3.17.252432`, package `com.google.android.gms`.
- GsfProxy `v0.1.0`, package `com.google.android.gsf`.
- FakeStore `v0.2.1`, package `com.android.vending`.

[`prepare-microg.py`](prepare-microg.py) downloads the APKs, verifies SHA256,
extracts arm64 native libraries, and generates privileged-permission allowlists
from each APK's binary manifest. It also supplies power/data-save exemptions.
[`install-microg.sh`](install-microg.sh) installs the prepared payload and grows
the system image by 512 MiB when necessary. It backs up the original image at
`/var/lib/waydroid/ztos-backup/microg/system.img` before modification.

Contrary to the initial task assumption, this image already contains LineageOS's
restricted microG signature-spoofing implementation in `ComputerEngine`. Its
`isDebuggable()` gate disables spoofing with the current Samsung `user` props.
[`patch-microg-spoofing.py`](patch-microg-spoofing.py) changes only that gate to
true. The original package-name, authentic microG signer, metadata and Google
certificate checks remain. It does not globally allow arbitrary signature
spoofing or modify WhatsApp's reported signature.

Patched `services.jar` SHA256:
`a175d6ace6cbfb315a571da34f07be146162dd9f76f59cdc08c0233e1ff53c18`.
The only changed DEX bytes are the four-code-unit gate prefix and its checksum /
SHA1 header. Every DEX entry is stored uncompressed and four-byte aligned.
A second patch invocation produces a byte-identical JAR.

References: [LineageOS implementation](https://github.com/LineageOS/android_frameworks_base/blob/lineage-20.0/services/core/java/com/android/server/pm/ComputerEngine.java),
[microG signature-spoofing guidance](https://github.com/microg/GmsCore/wiki/Signature-Spoofing),
[official GmsCore release](https://github.com/microg/GmsCore/releases/tag/v0.3.17.252432).
This uses the existing Lineage implementation; it does not add a separate modern
`hasSigningCertificate`/UID-certificate spoofing extension. The installed
release's Self-Check verifies its own supported checks; it does not prove every
possible caller API or Play Integrity verdict.

### Verification and installer metadata

All **18/18 microG Self-Check items passed**, including system spoofing, both
Google signatures, package presence, runtime permissions, cross-profile access,
and battery exemption. Evidence is in
[`results/microg-20261024/selfcheck.json`](results/microg-20261024/selfcheck.json),
with UI XML for all three pages and screenshots in that directory.

Restricted background-location and SMS permissions need a system allowlist
entry before `pm grant` takes effect. The narrowly scoped
[`MicrogRestrictedPermissions.java`](tests/MicrogRestrictedPermissions.java)
helper uses Android's PackageManager API to add and verify those two entries.
[`microg-grant-permissions.sh`](microg-grant-permissions.sh) grants the Self-Check
runtime permissions and sets overlay/battery exemptions. No SMS is sent by this
helper. Google device registration and Cloud Messaging were enabled in microG
Settings. Check-in succeeded. Device Attestation remains off.

WhatsApp remains UID `10125`, version `2.26.41.78`, with its original APK path.
Its installer now reads `com.android.vending`. `pm set-installer` was attempted
without reinstalling: root failed with `Unknown calling UID: 0`; Android shell
failed the new-installer certificate check. Therefore
[`set-whatsapp-installer.sh`](set-whatsapp-installer.sh) converted an exact
snapshot of Android's binary XML, changed only WhatsApp's `installer` attribute,
converted it back, stopped Android, and compared the live metadata against the
snapshot before replacing it. It verifies parsed metadata differs in exactly
that one attribute and preserves file ownership, mode and xattrs. Backup:
`/var/lib/waydroid/ztos-backup/microg/packages.before-installer.abx`.
No APK reinstall, app-data clear, or ATL modification was performed.

LXC attach still fails its host privilege check. All phone access used
`python /tmp/ztos-phone-control.py run|sudo|put|get`.
[`microg-android-shell.sh`](microg-android-shell.sh) enters Android's namespaces
through its zygote and inherits Android's environment. Inheriting that environment
is necessary for `app_process`, `uiautomator`, and ABX conversion tools.

### Reproduction and rollback

Prepare inside this repository:

```sh
python android/waydroid/prepare-microg.py android/waydroid/.microg-build
bash android/waydroid/tests/build-microg-permissions.sh android/waydroid/.microg-build/permissions
# prepare-microg.py also writes .microg-build/payload.tar.
```

Transfer the payload and scripts only through the authorized phone helper.
On the phone, place `install-microg.sh`, both patchers, and
`microg-android-shell.sh` together under `/var/tmp/wa-link/microg/`. Run as root:

```sh
bash /var/tmp/wa-link/microg/install-microg.sh /var/tmp/wa-link/microg/payload.tar
# Wait for sys.boot_completed=1 through microg-android-shell.sh.
# Copy MicrogRestrictedPermissions.dex and microg-grant-permissions.sh into
# /home/zerone/.local/share/waydroid/data/local/tmp/ on the host.
bash /var/tmp/wa-link/microg/microg-android-shell.sh /data/local/tmp/microg-grant-permissions.sh
bash /var/tmp/wa-link/microg/set-whatsapp-installer.sh
```

Enable Google device registration and Cloud Messaging in microG Settings, then
run the repo-side evidence check:

```sh
python android/waydroid/tests/verify_microg_selfcheck.py android/waydroid/results/microg-20261024
```

Image rollback is `bash install-microg.sh --rollback` as root on the phone.
It restores the pre-microG image while preserving Android data. It intentionally
does not restore a stale global package snapshot. To revert the installer later,
change only the current WhatsApp installer attribute back to `android` using the
same snapshot/compare procedure. Never overwrite newer package state with the
old full backup. Existing microG app data remains after image rollback.

### Ready for the next user attempt

Final cold-start readback retained the stock framework hash, patched services
hash, Play Store installer, Instagram disabled state, and both restricted runtime
grants. All **18/18 Self-Check items passed again after this restart**, using
fresh UI dumps with stale-file rejection. System permission XMLs and APK trees
are root-owned. Main microG Settings reports device registration **On**, Cloud
Messaging **On — 1 registered App**, and Device Attestation **Off**. This does
not establish that the registered app is WhatsApp or prove delivery end to end.

`/var/tmp/wa-link/RETRY` now requests the next attempt. Continuous capture is
`/var/tmp/wa-link/microg-retry.raw`; supervisor PID `56589` is recorded in
`/var/tmp/wa-link/microg-retry.pid`. The supervisor script
[`capture-microg-retry.sh`](capture-microg-retry.sh) reattaches after Android
restarts. Its error file was empty at handoff. This fresh capture had no matched
framework/services DEX alignment errors, missing-Play-Store / `SERVICE_INVALID`
errors, or KeyMint `-66` errors in the inspected window. No new registration or
companion-link verdict has been tested by the user yet. Leave Instagram disabled
until a successful user test justifies re-enabling it.

## Result (2026-10-24 ~15:20 UTC): registration accepted
Phone-number registration passed the "official WhatsApp" check and reached "Verify +94 ...:
use your other phone to confirm moving WhatsApp" (code requested). The working combination,
mirroring what ATL presents (ATLGStub fake GMS/GSF/Play Store with Google signatures; KeyStore
with empty certificate chains):
1. microG GmsCore + FakeStore with signature spoofing (services.jar patch, Self-Check 18/18).
2. WhatsApp installed with installer `com.android.vending` (`pm install -i com.android.vending`).
3. Stock build identity in waydroid.cfg [properties] (Samsung SM-G991B, user/release-keys,
   all partition fingerprints) instead of waydroid/lineage userdebug test-keys.
4. framework.jar attestation patch (apply-id-attestation.sh): no ID attestation, null challenge,
   so key generation succeeds without an attestation chain (stock failed with KeyMint -66).
Each alone was not enough; with all four the check passes. Instagram was disabled-user in the
container during the test (not shown to be required).


## Per-app Android 6 experiment (2026-10-24, 15:45 UTC onward)

The primary phone reports the working ATL companion as Android 6.0. This motivated
an SDK/release experiment; it does **not** establish which server check rejected
Waydroid. No successful companion link has yet been demonstrated with this variant.
The preceding registration result remains a separate observation.

### Implementation and preserved state

[`patch-id-attestation.py`](patch-id-attestation.py) now accepts `--old-sdk` and
forwards to [`patch-old-sdk.py`](patch-old-sdk.py). The existing no-attestation
patch remains in the input framework. `ActivityThread.handleBindApplication`
checks the exact package against comma-separated `persist.ztos.oldsdk` before
Application or ContentProvider creation. Only allowlisted processes get:

| Field on `Build.VERSION` | App value |
| --- | --- |
| `SDK_INT` / `SDK` | `23` / `"23"` |
| `RELEASE` | `"6.0"` |
| `RELEASE_OR_CODENAME` | `"6.0"` |
| `RELEASE_OR_PREVIEW_DISPLAY` | `"6.0"` |
| `SECURITY_PATCH` | `"2015-10-01"` |

These six fields are non-final. Framework readers use separate final fields on
`android.os.ZtPlatformVersion`, initialized from real system properties. The
collector inspected the live BOOTCLASSPATH and found 73 affected field references
across eight JARs: framework, telephony-common, IPsec, media, statsd, SDK extensions,
connectivity, and Wi-Fi. All were redirected; framework API-level decisions retain
SDK 33. `VERSION_CODES` constants remain unchanged. This protection covers the
collected boot libraries, not arbitrary third-party libraries or native callers.

DEX entries remain STORED and four-byte aligned. Other entries retain their original
compression. [`dex-hiddenapi.py`](dex-hiddenapi.py) preserves the original hidden-API
member flags across smali reordering; the older smali tool otherwise drops them.
Rebuild checks reject missing original members and verify retained flags.

Live framework SHA-256:
`586a26d3ed4b8c0c54287eb626b9db0cc1e7d161f5a53b80812b02b6a5136347`.
The microG signature-spoofing services.jar remains
`a175d6ace6cbfb315a571da34f07be146162dd9f76f59cdc08c0233e1ff53c18`.
Samsung identity, microG data, Play Store installer, Instagram disabled state, and
no-attestation behavior were retained. Global properties still report SDK 33 /
Android 13. The persisted allowlist contains **only `com.whatsapp`**.

### Checks and compatibility adjustment

The local probe includes both a ContentProvider and an Activity. With the package
outside the allowlist, both logged SDK 33 / release 13. After adding the probe and
restarting its process, both logged SDK 23, SDK string 23, all three release strings
6.0, and security patch 2015-10-01. The hook simultaneously logged frameworkSdk=33.
The probe was then removed from the allowlist and uninstalled.

WhatsApp itself logged `package=com.whatsapp sdk=23 release=6.0 frameworkSdk=33`
from its own process. Initial boot broadcasts exposed a compatibility problem:
WhatsApp selected its Android 6 background-service path, but system_server still
enforced its actual target SDK 36. Two early processes crashed with
`BackgroundServiceStartNotAllowedException` for ExternalMediaManager.
Adding only WhatsApp to the user device-idle whitelist prevented the reproduced
MEDIA_MOUNTED receiver crash. Background app-op modes were already allowed; those
were not changed. [`configure-old-sdk-runtime.sh`](configure-old-sdk-runtime.sh)
reproduces this exemption and records whether rollback must remove it. The original
WhatsApp user exemption was absent. This exemption can increase background activity.

Foreground launch, EULA, and companion QR rendering passed. Chats cannot be checked
before this unregistered installation links or registers. The initial boot also
logged a Launcher3 recents inflation failure; Launcher3 is outside the allowlist.
No claim is made here that every app path is compatible with the spoof.

The first deployment script returned status 1 from a services.jar hash check made
before rootfs remounted. Actual deployment and reboot succeeded; subsequent live
hash readback verified both JARs. The script now checks services.jar inside the
mounted image before unmounting.

Repeated shell starts then exposed stale framework boot artifacts: ART regenerated
a temporary framework extension each time. The checksum mismatch in the old
boot profile is evidence of stale precompiled inputs, not a new attestation error.
[`rebuild-old-sdk-boot.sh`](rebuild-old-sdk-boot.sh) builds one matching framework
extension from the patched JARs, retaining the unchanged ART core image.
[`install-old-sdk-boot.sh`](install-old-sdk-boot.sh) replaces the stale framework
extension group and snapshots its original files and symlinks for rollback.

### Reproduction and rollback

Use the phone-control wrapper for every transfer and command. Run collection as
root on the phone; build from the collected files in this repository:

```sh
# Phone, root; deploy both Python helpers alongside the entry point first:
python3 patch-id-attestation.py --old-sdk --collect oldsdk-before.tar
# Repository:
python3 android/waydroid/patch-id-attestation.py --old-sdk \
  --bundle android/waydroid/.oldsdk-build/before.tar \
  --output android/waydroid/.oldsdk-build/payload.tar \
  --tools android/waydroid/.microg-build/tingle/tools \
  --work android/waydroid/.oldsdk-build/bundle
# Phone, root, after transferring payload and scripts:
bash apply-old-sdk.sh oldsdk-payload.tar
# After Android boots, with the namespace helper available:
ANDROID_SHELL_HELPER=/var/tmp/wa-link/microg/microg-android-shell.sh \
  bash configure-old-sdk-runtime.sh
# Put rebuild-old-sdk-boot.sh into Android /data/local/tmp first:
bash /var/tmp/wa-link/microg/microg-android-shell.sh \
  -c 'sh /data/local/tmp/rebuild-old-sdk-boot.sh; echo $? > /data/local/tmp/ztos-oldsdk-boot/status'
bash install-old-sdk-boot.sh
```

`apply-old-sdk.sh --rollback` restores the eight original JARs, original boot
extension files when present, and only the previous allowlist property. It retains
other current configuration and Android data. After rollback boots, run
`configure-old-sdk-runtime.sh --rollback` with the same helper to remove the
exemption only if this experiment added it. The scripts do not clear WhatsApp.
Backups are `/var/lib/waydroid/ztos-backup/oldsdk`; never replace them with a newer
snapshot. Restore the original boot files and JARs together.

Evidence: [`results/oldsdk-20261024`](results/oldsdk-20261024). Build intermediates
are ignored under `.oldsdk-build/`. QR images and session contents are not evidence
to publish.

### Memory for later work

- Build.VERSION fields are shared with framework code inside each app process.
  Protect framework reads before changing app-visible values; framework constants
  already compiled into code require no rewriting.
- Boot-classpath changes require matching boot artifacts. A successful boot alone
  does not prove shell/app launches avoid repeated framework compilation.
- The current WhatsApp package requires API 23 and targets API 36. Its legacy code
  paths can conflict with Android 13 background-service policy.
- This phone's `lxc-attach` capability check fails. The existing
  `microg-android-shell.sh` enters the zygote namespaces and environment; use
  `--shell-uid` for PackageManager/UI operations. Never use plain SSH.
- WhatsApp is currently UID 10126. Do not reuse earlier capture UID 10125 blindly.
- This latest user request explicitly authorized `pm clear com.whatsapp`.
  It was executed at approximately 15:55 UTC; microG data was retained.

### Boot rebuild readback and pending handoff

At 16:02 UTC the matching extension was installed successfully (installer status 0).
The three new files are `boot-framework.art` (2,707,456 bytes), `.oat`
(1,831,792 bytes), and `.vdex` (628,860 bytes). The other stale framework extension
segments were backed up and removed; unchanged ART core segments remain.
Several subsequent shell/UI starts showed **zero dex2oat invocations**, zero profile
checksum mismatches, zero DEX alignment warnings, and zero DEX linkage errors.
One Launcher3 boot failure remains in that window; it is documented above and is
not a WhatsApp process crash.

A subsequent registration screen again displayed "Download the official WhatsApp
to continue" around 16:04 UTC. That result disproves any claim that this spoof has
already fixed registration. No phone number was entered by this agent. Another
`pm clear com.whatsapp` at approximately 16:05 UTC returned registration to its
initial state. The phone-number screen hid its normal overflow menu under the
SDK 23 branch. Starting the existing
`com.whatsapp/.companionmode.registration.ui.RegisterAsCompanionActivity` as Android
root opened the companion screen without modifying the APK. The earlier QR had
rendered successfully; its bottom was initially clipped, so a vertical swipe is
needed to bring the complete QR into view.

The phone-control connection became unavailable before the final post-rebuild QR
visibility check and RETRY creation. The last confirmed capture supervisor was PID
88053, writing `/var/tmp/wa-link/oldsdk-retry.raw`; this capture includes the earlier
buffer replay and should be filtered by the attempt's actual timestamp. RETRY is
still absent at this checkpoint. Do not interpret this checkpoint as a completed
handoff or a successful link. Resume with connection readback, QR visibility,
allowlist `com.whatsapp`, capture growth, and root-owned RETRY creation.
