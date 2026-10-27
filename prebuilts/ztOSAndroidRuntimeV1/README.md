# ztOSAndroidRuntimeV1

Prebuilt ARM64 Android Translation Layer runtime for ztOS. The OS installer
installs these packages directly; users do not need a compiler, Docker, or
the SSH builder. Package checksums are in `packages/SHA256SUMS`.

Contains ATL, standalone ART, bionic translation, OpenSL ES, VIXL, wolfSSL
JNI and BSD compatibility headers. Linux repository dependencies are still
installed by pacman. This is a package bundle, not one statically linked
executable. Android APKs may still require first-launch dex compilation.

Updated 2026-10-23. Source revisions and ztOS patches are pinned in
`android/atl/packages`; see `manifest.json` and `android/atl/README.md`.
Each archive includes its upstream license files. Keep the recipes and
patches with this bundle when distributing it.

Tested on the starlte ARM64 phone: Java execution, Android views, native JNI,
GLES rendering, WhatsApp welcome screen and companion QR screen. F-Droid and
Fossify Calculator reach their resumed activities on ATL21. WhatsApp compiles
on first launch and resumes. ATL21 supplies the missing TextView focus getter
that previously aborted chat layout, and cancels row long-press after 12 pixels
of movement. The updated app launch is verified; manual chat tapping and list
scrolling still need confirmation. Instagram reaches native startup, then
crashes in Meta analytics. General Android compatibility remains experimental.
The launcher disables title bars in the OS overlay.

The Instagram core shows its interrupted stack entering the app's Breakpad
signal multiplexer. ART then faults while unwinding that stack; the initial
native fault remains unknown.

ATL release 5 fixes coordinate conversion for nested custom Android views.
On-device verification covers touch down, movement and release, preserving
raw coordinates. The launcher defaults to 144 logical DPI for a smaller UI;
`ATL_DISPLAY_DENSITY=160` restores the previous size.

ATL release 6 also corrects SP/PX font units, text-size reporting and editable
text sizing. The matching ztMETRO source resizes Android apps above the keyboard
so their headers stay visible while typing.

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

ATL release 21 adds the missing stateful TextView soft-input-focus API and
cancels long-press after touch movement exceeds 12 pixels. Earlier ATL releases
add Android 9 APIs for Resources class loading, telecom service lookup,
key-generation policy and the main executor. Instagram passes those earlier
API blockers, but its native analytics thread still segfaults. The EGL
client-buffer fallback returns no GPU buffer and does not advertise unsupported
GPU import. Bionic release 6 provides that link fallback. ART release 4 adds
core-library desugaring for Java 8 time APIs; the standalone probe passes.
