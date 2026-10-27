# Waydroid Phase 2 results

Date: 2026-10-24 (Asia/Colombo). Target: Galaxy S9, Arch Linux ARM, aarch64.
Phone timestamps in the evidence are UTC on 2026-10-23.

## Deployment and recovery

Compared repository sources with `/home/zerone/ztmetro-phase2` and the installed
CLI/helpers. Files source already matched; ztMETRO, perfd and runtime helpers needed
updates. Built ztMETRO and perfd on the phone using the repository Makefile and its
native aarch64 toolchain. Files and app scanner targets were already current.

Before replacing installed files, retained `.phase2-finish-backup` copies beside
`ztmetro`, `ztfiles`, `ztapps`, `ztos-android`, `zrnperfd` and the Waydroid helpers.
The earlier `/usr/local/bin/ztmetro.phase2-backup` remains intact. Installed shell
updates by atomic rename, restarted through greetd, and verified the final process,
GLES2 renderer and 360x740 logical output. No shell rollback was needed. Existing
ATL runtime and Android user data were retained; no packages were uninstalled.

The runtime package remains `android_translation_layer 20261021.d658957d-48`.
ATL runtime code and its installed libraries were not changed.

## Missing probe: cause and fixes

The original 94,822-byte ATL probe declares `android:testOnly`. An explicit Android
package-manager diagnostic returned:

```
Failure [INSTALL_FAILED_TEST_ONLY: installPackageLI]
```

Waydroid's app-install command can return zero even when Android rejects the APK.
The old registry consequently recorded a nonexistent Waydroid installation. Repaired
that entry to ATL before retrying. Preserved its original APK on the phone as
`~/ztmetro-phase2/atl-probe-original.apk`.

Built a regular signed Java-only probe using
[tests/build_probe.py](tests/build_probe.py) and
[tests/probe/MainActivity.java](tests/probe/MainActivity.java). It retains the demo
package, resources and icon but clears `testOnly`. Installed it with
`ztos-android install ... --runtime waydroid`; both `waydroid app list` and the ztOS
registry now report `com.example.demo_application`. Its final registry runtime is
correctly `waydroid`; WhatsApp and Instagram remain ATL.

Additional fixes deployed during verification:

- Run privileged Waydroid install/remove operations as the desktop user, with that
  user's DBus environment. Root could not access the desktop session bus.
- Bound app-list post-checks and recover once on failure. An unavailable
  `waydroidplatform` previously left an unbounded verification process.
- Treat `waydroid.InputMethod` as an IME visibility signal, not another app. The
  Android IME surface stole focus and rendered blank. Its map/unmap now controls
  exactly one ztMETRO keyboard while Android retains app keyboard focus. This device
  did not expose text-input-v3 through Waydroid. Host typing reached Android.
- Recents force-stops the Android package without killing the shared Waydroid
  compositor process. IME surfaces do not become recents cards. Managed app names
  label Waydroid recents cards.
- Restore an existing hidden Waydroid window after direct CLI relaunch. Otherwise
  the container could thaw behind Home without another visible-window transition.
- Temporarily restore normal SIGCHLD handling around the synchronous screen-off
  helper. Ignored SIGCHLD produced a false `system()` failure despite completion.
- Resolve launcher icons through the manifest/resource table, including obfuscated
  WebP resources. WhatsApp's actual icon is `res/kkf.webp`; filename heuristics had
  selected the generic fallback. Convert WebP to PNG with the installed GdkPixbuf.

## Device verification

### Visual evidence correction (2026-10-24)

Review invalidated the original probe, keyboard and Back screenshots: they showed
only the ztMETRO lock screen. The original app-list screenshot did not contain the
probe. Those images did not establish visual success; the earlier visual pass
claim was incorrect. Screen-off relocking was a suspected cause, not proven from
those images alone.

Repeated the visual sequence with the screen on and an explicit control-interface
unlock. Replaced the affected PNGs after opening and inspecting each locally.
`verify_phone.py` now completes all captures before either screen-off test, resets
the idle timer with `screen on` before each capture, and reports captures as requiring
visual inspection rather than automatically declaring a visual pass. Its `main()`
guard also allows staged interactive checks without running screen-off tests.

The new [visual recheck log](results/visual-recheck.txt) records Home freeze with
unchanged `usage_usec=145901374`, recents-close freeze, normal screen-off freeze
(`1`), and `keep_awake` Home/screen-off checks (`0`). All captures preceded both
screen-off tests. The run then failed its added cleanup assertion: after restoring
configuration it tapped Home while already home and observed `0`, not `1`.
The script now relaunches before leaving, creating a fresh focus transition. See
[cleanup recheck](results/visual-cleanup-recheck.txt); the failed full-run log is
retained and is not presented as an entirely passing run. The focused cleanup
recheck passed (`cgroup.freeze=1`) and confirmed the temporary configuration was
removed, restoring its original absent state. Python compilation also passed.
The final script revision was checked through this focused cleanup run; the
entire sequence was not rerun after that last change.

Visual inspection confirmed no lock screen in the replacement captures. The
probe, single keyboard, typed `q`, details-to-Back transition, probe recents card,
empty recents after the staged dismissal, and probe icon in the app list are
visible. The recents-close PNG is from the staged check at 23:28 UTC; the other
replacement captures are from the ordered run at 23:29–23:30 UTC.

The ordered run also exposed rendering defects: the Back and recents captures
show a blank pale action-bar area where the probe title previously rendered.
The Home/frozen capture shows the app list with missing text/icon content.
These are recorded visual defects, not a clean graphics pass. Their cause remains
unverified; this follow-up changes only the verification script and evidence.

ATL screenshots, boost measurements, installation and process/task diagnostics
below are earlier evidence, not repeated visual checks in this follow-up. Internal
Android task removal, boost level and freeze values cannot be established from
PNGs; the report relies on the linked state logs for those claims. Prolonged
rendering reliability remains unverified.


Earlier state evidence: [device-checks.txt](results/device-checks.txt),
[deployment.txt](results/deployment.txt), [boost.txt](results/boost.txt), and
[ztmetro-final.log](results/ztmetro-final.log), and
[recents-task.txt](results/recents-task.txt). The frozen one-second sample held
`usage_usec` at 71,905,990.

The focused sequence is reproducible with
[tests/verify_phone.py](tests/verify_phone.py), run as the desktop user after deploying
these files. It restores the original `android.conf` after testing `keep_awake`.

- Both runtimes have managed app-list entries and icons:
  [Waydroid demo_application with Android icon and ATL calculator](results/app-list-waydroid.png),
  [ATL WhatsApp entries](results/app-list-atl.png). The separately installed native
  WhatsApp application remains separate. Generated `waydroid.*.desktop` app entries
  are excluded; the Waydroid runtime manager itself remains available.
- [Waydroid probe launched](results/waydroid-probe.png).
  [One host keyboard](results/waydroid-keyboard.png) appeared; typing `q` reached its
  EditText, visible in the later Back capture. Shell Back dismissed the keyboard;
  [details before Back](results/waydroid-details.png) and the following capture show
  the transition to “Back passed”:
  [Back passed](results/waydroid-back.png).
- [Recents](results/waydroid-recents.png) visibly contained the probe card.
  [After dismissing the card](results/waydroid-closed.png), recents was empty.
  These captures show no lock screen. The state check reported `cgroup.freeze=1`.
  Earlier process diagnostics reported that Android's package process ended while the container manager and shared compositor remained alive. Android
  can retain a historical recents record after force-stop; this is not a live task.
- Leaving Waydroid for Home set `cgroup.freeze=1`. Screen-off also froze before the
  display-off transition. Relaunch thawed the container and restored the existing
  app window. The exact physical display-off timing is not visually verified by
  these PNGs; screen-off status and freeze were checked through state files.
- With `keep_awake=com.example.demo_application`, Home and screen-off retained
  `cgroup.freeze=0`. Restored the original configuration after both checks. Frozen
  apps cannot deliver background notifications, sync or calls.
- zrnperfd observed Waydroid cgroup CPU load: a bounded Android workload changed
  balanced boost from `nominal` to `interactive`. Effective container CPUs remained
  `0-3`, the four efficiency cores.
- Launcher regression suite passed **12/12**, including obfuscated icon resolution
  and focus restoration after direct Waydroid launch. See
  [launcher-tests.txt](results/launcher-tests.txt). Shell syntax and Python compilation
  checks passed. Native builds completed with existing warnings.

## ATL R48 screenshots and visual findings

Launched both apps through `ztos-android launch`. Captured with `grim`; no ATL fixes
were made.

- [WhatsApp chat list](results/atl-r48-whatsapp-list.png): avatars, text and emoji
  render. The search background is visible but its hint is absent in this capture.
  Another filtered-list view showed the floating Ask Meta AI label overlapping a
  chat row and a new-chat button with multiple outlines.
- [WhatsApp group chat](results/atl-r48-whatsapp-chat.png): Today and unread-count
  backgrounds are rounded. The long group-membership system notice is broken: its
  small pale background does not expand to contain the multiline text, which runs
  toward the message composer. Some top message content is clipped behind the
  pinned banner. Emoji render, but this screen does not meet the no-overlap goal.
- [WhatsApp self chat and encryption notice](results/atl-r48-whatsapp-encryption.png):
  Today has a rounded background; the pale yellow encryption notice surrounds all
  three text lines with rounded ends. Existing message bubbles and voice messages
  remain visible without overlap in this view.
- [Instagram login](results/atl-r48-instagram-login.png): Mobile number or email,
  Password, Log in, Forgot password?, and Create new account are fully visible.
  Hints are readable and labels are not truncated. The blue login button has a
  conspicuous black outline; the large gray background may differ from intended
  app styling, but no reference rendering was supplied.

One initial WhatsApp launch crashed with SIGABRT and a reentered ART signal handler;
a subsequent launch opened the chat list and chats. This is an observed ATL runtime
limitation, not a Phase 2 fix. No messages were sent and no login was submitted.

## Remaining limits

The Phase 1 long-run/cold-start graphics stability limitation remains. wlroots also
warns about Waydroid committing zero-size initial window geometry. The short checks
here do not prove prolonged notification, graphics or suspend reliability. The replacement PNGs
also show the rendering defects recorded above. Existing
older registry entries can still have fallback app names/icons until refreshed by
installation; the probe and WhatsApp entries were refreshed and checked here.

ATL visual defects above remain intentionally unfixed, as required by the task.

## Launch-tap follow-up (2026-10-24)

Deployed a package-specific launch completion check and corrected boot-window
handling. Final verification used actual app-list taps through `ztmetro.ctl`,
with ztMETRO spawning the launcher. No verification launch used an SSH invocation
of `ztos-android launch`. SSH only prepared runtime state and injected touches.

### Findings and evidence limits

The original launch at compositor time `02:54:25.746` returned to Start after
30 seconds without an Instagram window. The retained root Waydroid log shows
successful unfreeze around wall time `02:14:45`; this was not a failure to thaw.
Compositor log timestamps are elapsed process time, whereas the CLI log uses wall
time. The original invocation had no persistent CLI output. Its exact Android-side
failure cannot be established retrospectively, and its complete no-window symptom
was not reproduced during this investigation. In particular, no evidence establishes
an interactive prompt or sudo password failure as its cause.

Two launch-contract defects were established:

1. A cold tap mapped the temporary app ID `Waydroid` before Instagram. The old
   compositor assigned the pending Instagram command to that surface, canceled
   loading, and returned to Start when it disappeared. In the observed pre-fix
   cold sequence, it showed `Waydroid` at `03:08:47.393`, returned to Start at
   `03:08:47.469`, and only showed Instagram at `03:09:08.699`. Thus loading feedback
   disappeared for approximately 21 seconds while Android continued starting.
2. The launcher accepted **any** nonzero `waydroid.open_windows` count as success.
   That condition cannot distinguish another app, a boot surface, or Instagram.
   Its subsequent one-shot focus request could arrive before Instagram mapped.
   This was a concrete false-success condition, but is not proof of the original
   unlogged invocation's precise failure. The new compositor acknowledgement
   removes that condition instead of assuming an arbitrary delay is sufficient.

The first instrumented frozen tap succeeded in approximately six seconds before
product fixes. It reported `tty=True`: ztMETRO inherited terminal stdin despite
calling `setsid()`. Launch children now receive `/dev/null` on stdin, and all final
Waydroid tap checks logged `tty=False`. Successful `sudo -n`, DBus, readiness and
launch stages are recorded in [launch-ztos-android.log](results/launch-ztos-android.log).

### Changes

- Keep temporary `Waydroid` boot surfaces out of app/recents ownership during a
  managed Android tap. They no longer consume the pending command or cancel
  loading. Handle a boot surface changing to a package app ID on a later commit.
  Explicit Waydroid full-UI launches retain their ordinary map path.
- Assign `waydroid.PACKAGE` windows their package launch command directly. An
  unrelated Waydroid window cannot consume another package's pending launch.
- Have ztMETRO acknowledge the package it actually shows in
  `$XDG_RUNTIME_DIR/ztos-waydroid-visible`. While holding its existing serialization
  lock, the launcher clears the old acknowledgement and requests package focus
  until that package is visible. Keep the launch marker through that check so idle
  freeze cannot race ahead of app presentation. A missing package window triggers
  the existing single recovery attempt. Outside ztMETRO, retain the native-window
  fallback.
- Append Waydroid helper stdout/stderr, elapsed shell trace, environment identifiers,
  TTY status and exit code to `~/.local/state/ztos-android.log`. Rotate at 8 MiB.
  A failed helper now produces a caught error and a shell notification pointing
  to that log, rather than an uncaught subprocess traceback.

### Final device checks

The final native build was deployed by atomic rename. Existing ztMETRO backups
remain, including `/usr/local/bin/ztmetro.launch-bug-backup` and the earlier
`.phase2-backup`. [Deployment helper](tests/deploy_launch_fix.sh) restores the
backup if shell restart verification fails; no rollback was needed. Final shell
PID was 151799, using panfrost/GLES2 at 360x740 logical coordinates. Installed and
built binary hashes match in [final state](results/launch-final-state.txt).

The following timings come from launch-to-map lines in the
[final compositor log](results/launch-final-ztmetro.log). Helper completion can
follow mapping by its polling interval. Instagram was force-stopped between warm
checks so both exercised a fresh CLI launch, rather than only restoring a window.
No app data was deleted.

| Starting state | Tap to mapped app | Inspected evidence |
|---|---:|---|
| Session STOPPED | 24.209 s | [Loading](results/launch-cold-loading.png), [Instagram login](results/launch-cold-app.png) |
| Session RUNNING, container FROZEN | 3.628 s | [Loading](results/launch-frozen-loading.png), [Instagram login](results/launch-frozen-app.png) |
| Session RUNNING, container RUNNING | 1.782 s | [Instagram splash at early capture](results/launch-running-loading.png), [Instagram login](results/launch-running-app.png) |
| ATL WhatsApp | 17.023 s | [WhatsApp chat list](results/launch-whatsapp-atl.png) |

All listed PNGs were opened and inspected. They show unlocked app/loading views.
The [Instagram row](results/launch-fixed-list.png) was tapped at logical
`160,570`. WhatsApp's two adjacent rows are visible in the
[selection capture](results/launch-whatsapp-list.png); the second row at `160,654`
is the managed ATL app. The compositor log confirms
`ztos-android launch com.whatsapp`, followed by app ID `com.whatsapp`.
No messages were sent and no Instagram login was submitted.

[Staged touch script](tests/launch_taps.py) reproduces the gestures and captures;
inspect the list before supplying its row coordinate. A preliminary navigation
attempt tapped Devices and Services before correcting list position; that capture
was not counted as an Android launch check. Intermediate compositor logs are
retained separately from final evidence.

Launcher tests passed **13/13**, including captured failure diagnostics and no
focus request on failure: [test output](results/launch-tests.txt). Shell syntax,
Python compilation and `git diff --check` passed. The native
[build log](results/launch-bug-build.log) retains existing warnings. ATL runtime
libraries, installed packages, registry runtime choices and Android user data were
not changed. The phone was returned to Start with `cgroup.freeze=1`, no launch
marker, and the verified shell still running.

These short checks establish the tested launch paths, not indefinite startup or
rendering reliability. One intermediate post-restart list capture again lost text
and icon texture content; the final selection captures rendered correctly. The
previously documented graphics limitation remains. The original historical
no-window failure's exact trigger remains unproven; persistent logs now preserve
future occurrences.

## WhatsApp dual-runtime follow-up (2026-10-24)

Both managed WhatsApp launchers are installed and pinned on Start:
**WhatsApp** uses ATL; **WhatsApp (Waydroid)** uses Waydroid. The existing native
WhatsApp desktop application remains separately installed. Its extra app-list row
is not a duplicate Android registry entry.

### Registry, CLI and shell changes

`applications.json` now keys entries as `PACKAGE@RUNTIME`. Legacy package-keyed
files migrate in place, defaulting missing runtimes to ATL. Migration first saves
`applications.json.pre-dual-backup`; old desktop files and pinned commands also
receive `.pre-dual-backup` copies. Repeated reads do not repeat migration.
All eight original registry entries retained every original metadata value;
the additional Waydroid WhatsApp entry brings the total to nine.

`launch PACKAGE --runtime atl|waydroid` and
`uninstall PACKAGE --runtime atl|waydroid` select one installation. A package
installed in only one runtime still launches without the option. An ambiguous
launch or removal reports that an explicit runtime is required. ATL launch
arguments remain supported, including after `--`. `list` prints both runtime rows.
Uninstall only removes the selected launcher's registry/desktop entry and retains
an APK/icon referenced by its peer; a failed Waydroid removal leaves registration
intact. Removal behavior was tested with temporary fixtures, not the phone's apps.

Desktop filenames are `ztos-android-PACKAGE-atl.desktop` and
`ztos-android-PACKAGE-waydroid.desktop`; their Exec commands always name a runtime.
The original ATL APK filename remains `com.whatsapp.apk`, because ATL derives
its saved-data directory from that filename. Waydroid uses a separate cached
`com.whatsapp-waydroid.apk`, so its updates cannot replace ATL's APK.
Both copies have the original SHA-256
`a6ef23a075243190255614f0ff71d1a39f935258a4eee9ef03e01823a3806748`.
The idle-policy registry readers accept both old and migrated keys.

ztMETRO matches managed windows by package **and runtime**. The app IDs
`com.whatsapp` and `waydroid.com.whatsapp` receive distinct launch commands,
Start-tile ownership and recents labels. App-list removal forwards its runtime.
Waydroid recents dismissal still force-stops only its Android package.
The previous launch fix's boot-surface handling, package-visible acknowledgement,
serialization, recovery retry, persistent diagnostics and `/dev/null` stdin remain.

### Deployment and verification

Built on the phone and installed ztMETRO by atomic rename. Shell restart passed;
PID 164028's running executable matches the installed and built binary. Backups
include `/usr/local/bin/ztmetro.whatsapp-dual-backup`, the corresponding CLI/power
helper backups, and `/usr/local/libexec/ztos-waydroid-network.whatsapp-dual-backup`.
Earlier launch-fix and Phase 2 backups remain. The
[deployment script](tests/deploy_dual.sh) restores the shell binary if restart
verification fails. No rollback was needed. A full launcher rollback must restore
the CLI, registry, old desktop files and pinned commands together; restoring only
the old CLI would leave it unable to read the migrated registry. No rollback
requires deleting Android app data or uninstalling WhatsApp.

Installed the existing phone APK using `ztos-android install ... --runtime waydroid`.
The helper confirmed Android's package list contained `com.whatsapp`.
The existing Waydroid data directory retained inode 1050831; it was not cleared.
ATL continued using `data/com.whatsapp.apk_` and opened its existing chat list.
No phone number was entered or verified, no onboarding agreement was accepted,
and no messages were sent.

All launches below used app-list or pinned-tile touches through `ztmetro.ctl`.
No verification launch invoked `ztos-android launch` over SSH. Screens were unlocked
before the sequence, kept on during captures, and every retained PNG was opened
and inspected:

- [App list](results/whatsapp-dual-list.png) and
  [return to list](results/whatsapp-dual-return-list.png): both managed runtime
  labels and icons, plus the separate native WhatsApp row.
- [ATL launch](results/whatsapp-dual-atl.png): existing chat list. The managed ATL
  row was tapped at logical `160,458`; launch-to-map time was 18.931 seconds.
- [Initial Waydroid loading](results/whatsapp-dual-waydroid-loading.png): still
  loading at the 30-second capture. Its first attempt produced no visible package
  window; the existing recovery retry restarted the session/container and succeeded.
  Launch-to-map time was 57.304 seconds; helper completion was about 58 seconds.
  This was an observed recovery path, not a clean first-attempt launch.
- [After recovery](results/whatsapp-dual-after-recovery.png) and
  [Waydroid screen](results/whatsapp-dual-waydroid.png): WhatsApp's custom-ROM
  alert over its welcome screen. The alert was left untouched.
- [Waydroid recents card](results/whatsapp-dual-recents-waydroid.png) and
  [ATL recents card](results/whatsapp-dual-recents-atl.png): separate cards titled
  “WhatsApp (Waydroid)” and “WhatsApp”, with different app contents. The compositor
  recorded two apps. A preliminary recents capture occurred before Waydroid mapped;
  it was replaced with the inspected two-app capture.
- [Pinned Start tiles](results/whatsapp-dual-tiles.png): both launchers side by side.
  The medium Waydroid tile visually truncates its long label; its stored label and
  full app-list/recents label are “WhatsApp (Waydroid)”. Tapping `180,540` restored
  [ATL chats](results/whatsapp-dual-tile-atl.png); after Home, tapping `290,540`
  restored the [Waydroid alert](results/whatsapp-dual-tile-waydroid.png).

Evidence: [state and preservation checks](results/whatsapp-dual-state.txt),
[install log](results/whatsapp-dual-install.log),
[Waydroid launch/recovery log](results/whatsapp-dual-launch.log),
[compositor log](results/whatsapp-dual-ztmetro.log), and
[native build warnings](results/whatsapp-dual-build.log).
[Tests](results/whatsapp-dual-tests.txt) passed **17/17**: 15 launcher tests and
2 shell-identity/idle-policy tests. Shell syntax, Python compilation and
`git diff --check` passed.

The phone was returned to Start with no launch marker and the shell still running.
The payload freeze check uses `/sys/fs/cgroup/lxc.payload.waydroid/cgroup.freeze`;
the earlier bare `0` samples in the state log measured the container-manager
service cgroup instead and are not payload-freeze results. The subsequent correct
payload samples, including the final read, were also `0`. A direct privileged
freeze briefly produced `1`, but a later read returned `0` again. Therefore this
run did **not** pass Home/idle freeze verification. Its cause and whether it predates
this follow-up remain unverified; it is not presented as a successful cleanup check.
Existing ATL missing-header/search-hint rendering defects remain visible. This
follow-up does not claim prolonged graphics reliability or first-attempt Waydroid
startup reliability after a compositor restart.

## First-launch and exit reliability follow-up (2026-10-24)

Phone access succeeded using only `python /tmp/ztos-phone-control.py` for
commands and transfers. The previous authentication blocker no longer applies.
All changes and evidence stayed in this repository and on the phone.

### Reproduced first-launch failure and fix

Two deliberate ztMETRO restarts reproduced the first-attempt no-window failure.
The old Waydroid session still answered DBus and Android readiness checks, and
`waydroid app launch com.whatsapp` returned successfully. Nevertheless, no package
window reached the new compositor before the visibility timeout. Both launches
needed the existing session/container recovery: 50.727 and 51.570 seconds from
the compositor's launch log to its first WhatsApp show log. See
[before-fix launch log](results/reliability-before-launch.log) and the
[two](results/reliability-restart-before.txt)
[touch traces](results/reliability-restart-before2.txt).

The compositor socket changed identity while the old session survived. The
launcher now remembers the socket device, inode and nanosecond change timestamp
in `$XDG_RUNTIME_DIR/ztos-waydroid-compositor`. If that identity changes, it stops
the stale session and replaces the container **before** issuing the first app
launch. Existing readiness checks and the bounded recovery fallback remain.
Normal same-compositor launches retain their session. This is an event-driven
check at launch, with no new daemon, timer or background polling.

This establishes a stale session across the compositor boundary as a reproducible
failure mechanism. It does not establish the precise native failure inside the
composer, or recover a stack trace from the original 57.304-second launch.
[Android logcat](results/reliability-startup-logcat.txt),
[restart logcat](results/reliability-restart-before-logcat.txt), and
[session journal](results/reliability-session-journal.txt) are retained.
One diagnostic used unsupported `waydroid logcat` options; subsequent captures
used `waydroid shell -- logcat` through the approved controller.

Deployment retained the original `.reliability-backup` files and a coordinated
`.reliability-policy-backup` set for `ztmetro`, `ztos-waydroid-launch` and
`ztos-waydroid-power` under `/usr/local/bin`. The native build and both helpers
were installed by atomic rename. [Deployment](results/reliability-deployment.txt)
verified shell restart, Panfrost and the installed binary hash.
[The deployment script](tests/deploy_reliability.sh) restores all three policy
backups and restarts the shell if verification fails. No rollback was needed.
Manual rollback must restore that coordinated set, then restart ztMETRO; the old
binary does not publish the new policy state. No app uninstall or user-data
deletion is needed. ATL libraries and data were not changed.

### Exit-policy ordering and background surfaces

The preliminary recents check exposed overlapping transitions: after dismissing
WhatsApp, Start appeared at `00:07:01.406`, then Android remapped the background
probe at `00:07:01.523`. Independent detached `active` and `idle` helpers could
finish out of order. The sample was frozen even though the probe had become
visible. See [pre-fix exit trace](results/reliability-exits-before.jsonl).
This is a demonstrated ordering hazard; it does not retrospectively prove which
process caused the historical dual-WhatsApp Home sample of `0`.

ztMETRO now atomically publishes its current Waydroid policy before spawning a
worker. Finite workers serialize through a runtime lock, read the latest policy,
and reconcile a policy change that occurred during an operation. A delayed
active worker cannot replay an old thaw after Home or screen-off. Launcher cleanup
also reconciles policy after removing its marker, covering an idle request that
was deferred during launch. No worker survives its event and no timer was added.
Screen-on checks the `waydroid.` app-ID prefix; an ATL app no longer thaws an
unrelated Waydroid payload merely because both are Android runtimes.

Unrequested Android package maps remain behind host Start or a foreground host
app. Explicit package launches and focus requests still show their window, and
Android navigation while a Waydroid app is foregrounded retains its map path.
Thus closing one Android task cannot let a background task take Start's focus.

WhatsApp's welcome activity retained its window after shell Back and Android
Back during the preliminary check. That is not a completed exit and its `0`
freeze sample is not a Home-policy failure. The original probe also retained its
root window. The probe fixture now explicitly calls `finish()` on root Back;
its details-page Back handler remains unchanged. Back-to-Start trials use this
probe; Home, recents and screen-off use WhatsApp. Both configuration settings use
`keep_awake=com.whatsapp` when enabled. The existing policy treats a registered
Waydroid package in that list as keeping the shared payload awake.

Built and signed the fixture with the existing test key, then updated it in place.
The previous APK and registry were backed up as
`~/ztmetro-phase2/probe.reliability-backup.apk` and
`applications.reliability-backup.json`. No package was uninstalled and no app data
was cleared. [Build](results/reliability-probe-build.txt),
[install](results/reliability-probe-install.txt), and the
[focused Back check](results/reliability-probe-back-fixed.txt) are retained.
The inspected [Back-to-Start capture](results/reliability-probe-back-fixed.png)
shows unlocked Start; the accompanying state check reports freeze `1`.

### Measurements

The first five baseline Home exits all reported payload `cgroup.freeze=1` at
both 3 and 30 seconds. Final repeated checks are recorded below.

The final [40-trial exit trace](results/reliability-final-exits.jsonl) records
**80/80 matching payload samples**. Each table entry represents five trials;
values are `cgroup.freeze` at 3 seconds / 30 seconds after the exit gesture.
The payload path is `/sys/fs/cgroup/lxc.payload.waydroid/cgroup.freeze`, never
the container-manager service cgroup.

| Exit | keep_awake off: 3 s / 30 s | keep_awake WhatsApp on: 3 s / 30 s | Trials per setting |
| --- | --- | --- | ---: |
| Home, WhatsApp | 1 / 1 | 0 / 0 | 5 |
| Back to Start, probe | 1 / 1 | 0 / 0 | 5 |
| Recents close, WhatsApp | 1 / 1 | 0 / 0 | 5 |
| Screen off, WhatsApp | 1 / 1 | 0 / 0 | 5 |

The recorded compositor segments confirm Start for Home, Back and recents,
and completed screen-off policy for screen-off. No package was shown after
any recorded exit. The configuration was restored to its original absent state
before the launch matrix. The probe's explicit root finish is a test-fixture
choice, not a claim that WhatsApp's welcome screen now exits on Back.

[The phone runner](tests/reliability_phone.py) performs finite trials, uses
control-interface touches, and keeps raw observations. It waits for Home's
animation before another tile tap. Back trials use the control-interface launch
command for the probe; the WhatsApp timing trials use only actual tile taps.
No verification WhatsApp launch invokes `ztos-android launch` from a remote shell.

### Final tile-launch timings

All **20/20 first app-launch attempts mapped without a recovery retry** after
both the launcher and native policy fixes were deployed. All starting states
were checked before tapping: STOPPED, payload FROZEN, payload RUNNING, or the
first launch after a deliberate ztMETRO restart. The warm checks force-stopped
WhatsApp between trials so they exercised fresh launches, not cached window
restoration. Android data was retained.

These are monotonic observations from writing the actual `tap 180 540` control
command to detecting the package's first compositor show log. The runner checks
that log at 100 ms intervals, so the numbers include observation delay. They
measure first native-window presentation, which can be a splash, not full UI
readiness. The summary also retains independent compositor launch-to-show
intervals. Post-restart trials wait eight seconds for the shell, unlock, then
make their first app tap; the restart wait is outside tap-to-map timing.

| Starting state | Five tap-to-map observations, seconds | Recovery retries |
| --- | --- | ---: |
| Session STOPPED | 32.154, 31.534, 29.721, 27.177, 28.269 | 0 |
| Session RUNNING, payload FROZEN | 6.252, 6.859, 7.054, 11.416, 5.843 | 0 |
| Session RUNNING, payload RUNNING | 9.276, 2.830, 2.637, 3.342, 4.140 | 0 |
| First launch after ztMETRO restart | 32.615, 31.848, 32.141, 29.436, 34.600 | 0 |

The five restart trials each took the deliberate compositor-generation preflight:
the stale session/container was replaced before the first app command. This
avoids the failed attempt and its 20-second visibility wait; it still pays an
Android cold-start cost. No retry was hidden inside these measurements.

Evidence: [raw launch trials](results/reliability-final-launches.jsonl),
[complete launcher diagnostics](results/reliability-final-launch.log), and
[validated summary](results/reliability-summary.json).
[The evidence checker](tests/summarize_reliability.py) verifies all eight five-trial
exit groups, all four five-trial launch groups, starting states, package mapping,
one visibility-wait attempt per launch, successful helper exits and exactly five
compositor-change preflights.

Every generated PNG was opened and inspected. The final
[stopped](results/reliability-launch-stopped.png) and
[frozen](results/reliability-launch-frozen.png) captures show WhatsApp's ROM notice
over its welcome screen. The final
[running](results/reliability-launch-running.png) and
[post-restart](results/reliability-launch-restart.png) captures show blank app
content despite a package mapping and acknowledgement. The intermediate
[running capture](results/reliability-intermediate-launch-running.png) lost its
WhatsApp logo; the [early Start capture](results/reliability-app.png) lost tile
text and icons. These are rendering failures, not successful visual app checks.
The original long-run rendering limitation remains unresolved; the measurements
establish window mapping and freeze policy, not consistently rendered or usable
app contents. No onboarding agreement was accepted, no phone number was entered,
and no messages were sent.

### Audio, ordering and ATL regression checks

The [frozen audio-server restart](results/reliability-audio.txt) restarted
PipeWire, Pulse and WirePlumber through the user manager. Payload freeze stayed
`1` before, at 3 seconds and at 30 seconds; `usage_usec` stayed exactly
`81835867`, and the saved HAL generation stayed identical. The reconnect oneshot
finished inactive with `MainPID=0`. The installed audio helper and latency
configuration hashes match repository sources. The helper's FROZEN guard and the
node-only latency rule therefore agree with this live no-thaw result. This is a
bounded event test, not a claim that unrelated phone services generate no wakeups.
See [the finite verifier](tests/verify_reliability_audio.py).

[Focused ordering checks](results/reliability-ordering.txt) repeated the exact
background-task remap: open the probe, open WhatsApp, then dismiss WhatsApp from
recents. In both configurations the compositor logged the probe map being kept
behind host Start. Samples were `1 / 1` with keep_awake off and `0 / 0` with it on.
A cached-app tap followed by Home after 20 ms also finished at Start with `1 / 1`.
[The focused verifier](tests/verify_reliability_ordering.py) passed. The inspected
[off](results/reliability-background-off.png) image shows rendered Start; the
[on](results/reliability-background-on.png) and
[rapid Home](results/reliability-rapid-home.png) images show Start tile backgrounds
with missing text/icons. Those images establish the continuing graphics defect,
not a clean visual pass.

[ATL regression evidence](results/reliability-atl.txt) records calculator
`org.fossify.math` mapping through ATL while the Waydroid payload stayed frozen.
Screen-off and screen-on each retained freeze `1` at three seconds. This validates
the changed screen-on runtime distinction without changing ATL libraries or data.
It is a state/window check, not a complete calculator rendering test.

[Automated regressions](results/reliability-tests.txt) passed **23/23**: 15 CLI
checks, two dual-identity checks, three compositor-generation checks and three
power-ordering checks. The latter cover a stale idle worker, Home during thaw,
and marker cleanup after a deferred idle request. Shell syntax, Python compilation,
native [build](results/reliability-build.log), recorded-evidence validation and
`git diff --check` passed. Existing native compiler warnings remain in the build log.

### Final cleanup and preservation

The [cleanup restart](results/reliability-final-restart.txt) reinstalled the same
verified binaries and confirmed their unchanged hashes, Panfrost and shell startup;
no rollback was needed. Restart and configuration reload alone did not consistently
restore graphics: the inspected [restart](results/reliability-cleanup.png) and
[reload](results/reliability-cleanup-reload.png) captures still lack Start text and
icons. A subsequent normal screen-off/on cycle produced
[readable, unlocked Start](results/reliability-cleanup-screen-cycle.png).
That recovery is a cleanup observation, not a fix for the underlying rendering
problem. The earlier [ATL-check Home capture](results/reliability-final-home.png)
had also rendered correctly before this final restart.

[Preservation checks](results/reliability-preservation.txt) retain the original
WhatsApp data-directory inodes: Waydroid `1050831`, ATL `393716`. The installed
`ztos-android` CLI hash is unchanged from the dual-runtime follow-up. All registry
keys and every non-probe entry are preserved; only the probe fixture entry changed.
Android user data was never cleared, no package was uninstalled, and the original
probe APK remains available for rollback.

The [final state check](results/reliability-final-state.txt) found configuration
restored to its original absent state, no launch marker, no reliability test
runner, and shell PID `314106`. Payload freeze stayed `1` and `usage_usec` stayed
`120250578` across a final 30-second idle observation. ATL remains R48 and the
audio reconnect oneshot is inactive with `MainPID=0`. After the inspected readable
Start capture, the display was turned off for unattended idle; payload freeze
was checked again at `1`. The deployed fixes and all rollback backups remain.
