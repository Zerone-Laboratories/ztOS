# Android Translation Layer on ztOS

ATL runs Android code in a Linux process: standalone ART supplies the Java
runtime, bionic_translation bridges native Android libraries, and ATL maps
Android APIs and windows to Linux/GTK/Wayland. It does not boot Android or
replace ztMETRO. Android application compatibility remains experimental.
This build targets ARM64 ztOS: Java-only and arm64-v8a APKs. An APK containing
only ARM32 or x86 native libraries is rejected with an explanation.

Upstream: [Android Translation Layer](https://gitlab.com/android_translation_layer/android_translation_layer)
and [build instructions](https://gitlab.com/android_translation_layer/android_translation_layer/-/blob/master/doc/Build.md).
The original requested guide is [Nura's ATL wiki](https://wiki.nura.eco/wiki/Android_Translation_Layer).

## Build on the SSH machine

Use `android/atl/build.sh` on a Docker-capable Linux builder. It builds ARM64
packages in an isolated Arch Linux ARM container, using the pinned source
archives and checksums in `packages/`. On an x86 builder, binfmt/QEMU runs the
ARM64 toolchain; the resulting programs are ARM64, rather than host binaries.

```sh
ATL_ROOTFS=/path/to/ArchLinuxARM-aarch64.tar.gz \
ATL_WORKDIR="$PWD/build/atl" android/atl/build.sh
```

An existing `alarm-aarch64:base` Docker image can be reused. `ATL_BASE_IMAGE`,
`ATL_CONTAINER`, `ATL_CPUS` and `ATL_MEMORY` configure the builder. Defaults
limit the container to 12 CPUs and 16 GiB. Sources and intermediate objects
are retained for incremental builds; logs are under the work directory.
Do not use a rootfs for a different architecture.

The ARM build adds VIXL, required by ART's ARM code generators but missing
from the original AUR dependency list. ART's GCC header compatibility fix is
limited to C++, so it does not inject C typedefs into ARM assembly. ATL's
optional layer-shell keyboard is disabled: GTK uses ztMETRO's text-input-v3
keyboard. The launcher selects Wayland and GTK's GLES renderer for Panfrost, and disables
ATL window title bars so Android apps use the available phone screen.

## Install

The repository ships the tested binary bundle as
`prebuilts/ztOSAndroidRuntimeV1/packages`. The OS installer uses it
automatically, without rebuilding ART or ATL. For a manual installation,
copy that directory (including `SHA256SUMS`) to the phone and run as root:

```sh
bash android/atl/install-runtime.sh /path/to/packages
```

The script verifies archive hashes and package architectures, installs the
repository dependencies, then installs the custom runtime packages together.
The OS overlay supplies `/usr/local/bin/ztos-android` and APK MIME handling.
The regular OS installer prefers `prebuilts/ztOSAndroidRuntimeV1/packages`.
If that bundle is absent, it can use locally built packages from
`build/atl/packages`; if neither exists, it reports the missing runtime.
The prebuilt bundle contains ARM64 executables and shared libraries, plus
Java framework code. Repository dependencies still install through pacman.
APKs can still require first-launch dex compilation; the prebuilt runtime
avoids rebuilding the runtime itself during OS installation.

## Applications

```sh
ztos-android doctor
ztos-android install ~/Downloads/application.apk
ztos-android list
ztos-android launch com.example.application
ztos-android run ~/Downloads/application.apk
ztos-android remove com.example.application
```

Installing copies the APK into the user's data directory and creates a
normal desktop entry, which ztMETRO's existing app-menu scanner discovers.
Reinstalling the same package updates that APK and menu entry. App data
lives under `~/.local/share/ztos/android/data` and is retained on removal.
`open FILE.apk`, used by APK MIME handling, installs and launches it.
Additional ATL options follow the APK/package argument, for example
`ztos-android run game.apk -l org/example/GameActivity`.

These are native user processes with the user's Linux permissions, not an
Android security sandbox. There is no promise that arbitrary Play Store
apps, Google services, or all Android APIs work.

The launcher defaults to 144 logical DPI, making Android layouts 10% smaller
than the previous 160 DPI default. Override it per launch, for example
`ATL_DISPLAY_DENSITY=160 ztos-android launch com.whatsapp`. Display metrics
and Android configuration use the same density. Nested custom Android views
receive touch coordinates in their own coordinate space; raw screen positions
and the caller's event coordinates are preserved.

On-device verification of ATL release 5 (2026-10-22): a control nested at
offset (60, 140) did not receive a tap on release 4. The same tap at Android
coordinates (100, 200) now reaches it at local coordinates (40, 60). Movement
and release also retain the expected local and raw coordinates, without the
previous GTK hit-testing errors. Display metrics and configuration both report
144 DPI. RSA/ECDSA OID signature verification and tamper rejection also pass.

ATL release 6 fixes text sizing: SP values resolve through scaled density,
explicit PX values remain pixels, and XML dimensions are not scaled twice.
`getTextSize()` and `getPaint()` agree with rendered size. Editable text uses
the same sizing path, including placeholder text. ztMETRO identifies ATL
clients by executable and resizes their viewport above the keyboard instead
of moving the entire window, keeping the chat header visible.

App stdout and stderr, including ART exceptions and native crash messages,
are saved to `~/.local/state/ztos/android/PACKAGE.log` (or under
`$XDG_STATE_HOME/ztos/android`). A log over 8 MiB is rotated to `.log.1`
at the next launch. These logs can contain private application details.

The runtime ships a built-in `CryptoProvider` within ATL's `api-impl.jar`.
Standalone ART already ships the Bouncy Castle ECDSA/RSA implementations, but its
stripped provider does not advertise them. The ztOS provider registers those
mappings during application startup so applications can verify modern TLS certificate
signatures (e.g. SHA256withECDSA); it does not disable verification or alter the ART boot image.

## Verification

```sh
python -m unittest discover -s android/atl/tests -v
bash -n android/atl/*.sh android/atl/packages/*/PKGBUILD
```

The launcher tests cover UTF-8/UTF-16 binary manifests, malformed input,
unsupported native ABIs, installation, menu entries and updates. Runtime
validation must additionally check ELF architecture, linked libraries, ART
startup, and an actual APK window on the phone. Build artifacts, test APKs,
and runtime logs belong in `build/atl`, outside version control.

### Current app check (2026-10-23)

ATL release 21 adds Android 9 API compatibility for `Resources` class loading,
telecom service lookup, key-generation policy and `Context.getMainExecutor()`.
It also implements `TextView.getShowSoftInputOnFocus()` with Android's default
value, fixing WhatsApp's fatal layout-time `NoSuchMethodError`. Its GTK
long-press recognizer now cancels after touch movement exceeds 12 pixels, so
scrolling a chat list cannot become a row long-press. F-Droid and Fossify
Calculator reach `onResume` and window focus on the starlte phone. WhatsApp
launches and reaches `onResume` on release 21; manual chat tapping and scrolling
still need confirmation on the phone. Instagram advances through the repaired
Java API startup paths, then faults in its native `Analytics2-Even` thread. Its
interrupted stack enters Instagram's `libbreakpad.so` signal multiplexer; ART's
signal handler then faults while unwinding that stack. The initial signal cause
remains unknown, so broad Instagram compatibility remains unresolved.

ART release 4 includes `core-oj` desugaring for Java 8 time APIs. The standalone
ART probe passes. Bionic release 6 resolves Instagram's missing EGL client
buffer symbol with a null-returning fallback; it does not advertise or emulate
GPU import for CPU-backed buffers. Package hashes and the fresh-install bundle
are updated in `prebuilts/ztOSAndroidRuntimeV1`.

### Phone validation (2026-10-21)

Built on the x86 SSH builder and installed on the ARM64 starlte phone:

- ART executed a dex Java smoke test and returned exit status 0.
- The upstream demo APK displayed its Android TextViews in ztMETRO.
- Gravity Defied displayed its animated menu.
- GLES3JNI loaded its arm64-v8a library and rendered the rotating colored tiles.
- `ztos-android doctor` found ARM64 binaries and no missing linked libraries.
- The supplied WhatsApp APK reached its welcome screen after about seven
  minutes of first-launch dex compilation. The next launch reused that cache.
  Registration and messaging were not tested: the phone had no internet route.

Screenshots and logs are in `build/atl/evidence`; the seven verified runtime
archives and their checksum manifest are in `build/atl/packages`. These tests
establish Java, Android views, native JNI and GLES operation. They do not
establish arbitrary app compatibility, notifications, Google services, or
full Android keyboard/API behavior.

The ART recipe also fixes `JNI_OnUnload` aborting when shutdown has already
detached the current thread. Normal loading still checks its JNI environment.

### WhatsApp companion linking

Package release 3 also registers the standard numeric RSA/ECDSA signature
identifiers, including `1.2.840.113549.1.1.11` (SHA256withRSA). WhatsApp's
Bouncy Castle certificate validator uses these identifiers rather than the
algorithm names. Without the aliases, it rejects its server certificate
chain and can abandon companion setup during chat sync. On-device checks
now validate the live WhatsApp certificate chain through Bouncy Castle and
reject a deliberately damaged certificate signature. Completing chat sync
and messaging still requires a linked-device retest.

The supplied APK initially terminated its ConnectionThread while generating
`aes_auth_key`: ATL delegated AES key generation to a BC provider that has
AES ciphers but no AES KeyGenerator service. Package release 2 generates AES
key bytes with SecureRandom, validates 128/192/256-bit sizes, and stores the
result through ATL's existing keystore path. It retains the Android default
of 256 bits when no size is specified. No encryption or authentication checks
are bypassed. ATL's keystore remains software based.

On-phone retesting reached `RegisterAsCompanionActivity`, resumed, and
rendered the companion-device QR code without the original exception.
Completing a link with a primary phone has not been verified. The current
layout clips part of that screen; QR layout/scrolling needs further testing.

### APK dependencies and first launch

Installing/opening an APK checks the declared Linux runtime dependencies and
linked libraries. Missing runtime packages are installed through the fixed
`ztos-android-dependencies` helper and ztOS's existing authentication sheet.
The installer retains the V1 package bundle under
`/usr/local/share/ztos/android-runtime` for repair. Repository dependencies
need network access when absent. APK-supplied package names or commands are
never passed to the privileged helper. Missing Android API implementations
are compatibility work, not dependencies that pacman can automatically fix.

ART's validated on-disk cache is reused on later starts. APK/framework/runtime
updates or a cleared/invalid cache can require compilation again; it is unsafe
to skip validation just because an app has run once. A per-package launch lock
prevents concurrent app launches from compiling the same APK twice.

During actual dex2oat activity, the shell shows **Compiling with
ztOSAndroidRuntimeV1**, an indeterminate progress bar, and **Stop**. ART does
not report a reliable percentage. The compiler holds a renewable rapid-boost
lease only during compilation; completion, cancellation, or a dead monitor
releases it. Stop terminates the launched process group, including its
compiler, then returns to Start. APKs and app data are retained; ART validates
or regenerates any unfinished cache on the next start. The normal Loading
state returns when compilation ends. The watcher exits after 30 seconds
without compiler activity so it does not poll throughout the app's lifetime.

On-phone validation: dependency checks passed; compilation UI, Stop, process
cleanup and rapid-to-normal release were tested with a temporary compiler
lifecycle fixture. Existing APK/ABI tests plus dependency repair/failure and
compiler descendant lifecycle tests pass. See `build/atl/evidence/compiling.png`.

ATL release 7 isolates GTK snapshot frames, including unbalanced saves and
root clips. Opaque vector drawables retain vector paths and tint instead of
using small bitmap caches; translucent root alpha retains grouped rendering.
Failed image decoding returns null with invalid output dimensions instead of
passing a null pixbuf to GTK. On-device probes verify three clean frames,
opaque/translucent paths, WebP decoding and malformed-image handling. These
changes improve rendering; incomplete Android APIs can still affect app UI.

ATL release 8 routes shell Escape/Back to the current Android activity and
uses ACTION_DOWN for physical Enter editor callbacks. Touching Button and
ImageButton widgets preserves editor focus, preventing keyboard dismissal
and layout movement during a send tap. The matching compositor dismisses
the Android keyboard on the first Back press before navigating. On-device
fixture checks cover one-click activation, keyboard retention, valid Enter
action and activity Back dispatch; no messages were sent by the tests.
