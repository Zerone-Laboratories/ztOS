# zrn-pushd: push messages for Android apps on ztOS

Android apps hear about a new message by push: Google keeps one TLS
connection open to the phone (MCS, `mtalk.google.com:5228`) and sends the
message down it; the app is woken for it and fetches the rest itself.

`zrn-pushd` holds that connection for the whole phone, outside Android, and
hands each message to the app in whichever runtime it lives in:

- **Waydroid** apps register through microG, which is installed in the
  Android image. A message for one of them thaws Android, is broadcast to the
  app exactly as microG would broadcast it, and Android goes back to the
  shell's freeze policy a little later.
- **ATL** apps (ztOSAndroidRuntimeV1) are plain Linux processes. Their
  REGISTER intent goes over D-Bus to `com.google.android.gms`, which is
  `zrn-pushd`. It registers them with Google itself and delivers their
  messages over D-Bus.

So Android can stay frozen while nothing is in front, and a chat app does not
have to be kept awake (the old `keep_awake=` exemption) to get its messages.

## How it fits together

| Part | Where | What |
|---|---|---|
| `zrn-pushd` | `/usr/local/bin`, user unit `zrn-pushd.service` | the MCS connection, ATL registration, delivery |
| `zrn-pushd-waydroid` | `/usr/local/lib/zrn-pushd` | thaws Waydroid (or starts its session) and hands one message over |
| `ztos-waydroid-recover push PKG` | `android/waydroid/recover.sh` (root) | `am broadcast` of the message inside Android |
| `ztos-waydroid-recover microg` | the same (root) | microG's device identity; microG's own MCS kept off |
| `ztos-waydroid-power` | `android/waydroid/power.sh` | no freeze while a push hold is on |
| `ztos-android` | `os/overlay/usr/local/bin` | ATL apps get their package as GApplication id, and a D-Bus service file |
| `/etc/sudoers.d/zrn-pushd` | written by `install.sh` | the user may run the two root actions above |

### One device identity

Google's push service knows a phone by the androidId and securityToken it got
at check-in. microG checked in inside Waydroid, and every Waydroid app's
registration (register3) is tied to that identity, so `zrn-pushd` logs in with
the same one. It reads it from microG's settings provider through the root
helper and keeps a copy, mode 0600, in `~/.local/state/zrn-pushd/identity`.
It is never written anywhere else, logged, or committed.

Only one MCS connection per device can be up: a second login closes the
first. So microG's own connection is kept off, while its registration keeps
working. In microG 0.3.17 (`GcmPrefs`, read from the shipped APK):

- `gcm_enable_mcs_service` (`isEnabled`) is all that registration
  (`PushRegisterService`, `PushRegisterManager`) checks;
- the MCS connection is started only when `isEnabledFor(network)` holds,
  which needs the network type's heartbeat to be non-negative:
  `gcm_network_{mobile,wifi,roaming,other}` is minutes, **0 is automatic**
  (connects), and **-1 is off**.

`ztos-waydroid-recover microg` sets all four to -1, keeps
`gcm_enable_mcs_service` true and `gcm_confirm_new_apps` false (every app may
register without a prompt). It does this every time `zrn-pushd` starts. No
firewall rule is needed. To give microG its connection back, stop
`zrn-pushd` and set the four values to 0 (microG's push settings screen, or
`content update` on `content://com.google.android.gms.microg.settings/gcm`).

### The connection

`mtalk.google.com:5228`, TLS (verified against the system CAs and the host
name), protocol version 41, and the LoginRequest microG sends (`android-33`,
`mcs.android.com`, `use_rmq2`, `auth_service` ANDROID_ID). Then:

- HeartbeatPing every 10 minutes (Google's LoginResponse may make it shorter).
  When one goes unanswered for 60 s, the interval drops by a third, down to
  3 minutes, and is kept in `~/.local/state/zrn-pushd/heartbeat-ms`. The
  timer is `CLOCK_BOOTTIME_ALARM`, so it wakes a sleeping phone; `make
  install` gives the binary `cap_wake_alarm`. Without the capability it
  falls back to `CLOCK_BOOTTIME`.
- When the network changes it reconnects without waiting. rtnetlink reports
  address changes: losing our own address means reconnect now; a new address
  while offline means try now; any change while online sends a heartbeat to
  check. Otherwise it backs off from 2 s to 5 minutes.
- Server pings are answered, and `IdleNotification` is answered "false", as
  microG does.

A message (DataMessageStanza) is acknowledged only once an app has taken it.
That means a selective-ack IqStanza now and its persistent id in the next
login's `received_persistent_id`; the ids are kept in
`~/.local/state/zrn-pushd/persistent-ids` until a login carries them. A
message that cannot be delivered is retried 3 times (30 s, 60 s, 120 s). If
it still fails it is left unacknowledged, and Google sends it again at the
next login.

### Waydroid apps

microG registers them (gcmstatus `registrations`). A message whose category
is not an ATL registration goes to Waydroid:

1. `zrn-pushd` writes `$XDG_RUNTIME_DIR/ztos-waydroid-push-hold` (an epoch).
   While it holds, `ztos-waydroid-power` does not freeze Android.
2. `zrn-pushd-waydroid` runs `ztos-waydroid-recover unfreeze`. If Android is
   not booted, it starts the Waydroid session and waits for `ready`.
3. `ztos-waydroid-recover push PKG` checks that the package has a
   `c2dm.intent.RECEIVE` receiver (exit 4 if it has none). It puts the package
   on the temporary power allowlist for 60 s (`cmd deviceidle tempwhitelist`,
   as microG's `addPowerSaveTempWhitelistApp`). Then it runs `am broadcast` of
   RECEIVE to the package with `FLAG_RECEIVER_FOREGROUND` and
   `FLAG_INCLUDE_STOPPED_PACKAGES`, because ztOS closes apps with
   force-stop. `am` returns when the receiver has run. The extras are
   microG's: `from`, `google.message_id` (the persistent id when there is
   one), `google.sent_time`, `google.ttl`, `collapse_key`, `gcm.rawData64`,
   and the data, without `from` and `google.*` (except `google.c.*`).
4. After the last delivery the hold lasts 45 s more, so the app can fetch its
   messages. Then `ztos-waydroid-power sync` applies the shell's policy again
   (frozen when idle or the screen is off).

microG has no way for another process to inject a message once its MCS is
off. So the broadcast is the delivery, and it is the same broadcast microG's
`McsService.handleAppMessage` sends.

### ATL apps

ATL sends `com.google.android.c2dm.intent.REGISTER` (from `startService` or
the Firebase Messenger `bindService`) to `com.google.android.gms`, object
`/com/google/android/gms`. It uses `org.gtk.Actions.Activate` (or
`org.freedesktop.Application.ActivateAction`) with action `startService` and
the intent `(action, className, data, extras, sender_dbus_name)` (ATL's
`doc/DBusIntentApi.md`). The unit is D-Bus activatable
(`/usr/share/dbus-1/services/com.google.android.gms.service`).

`zrn-pushd` finds the app's APK from the caller's command line (ATL is
started with the APK's path), or from `~/.local/share/ztos/android/apks/`.
It reads the package, version code, target SDK and the SHA-1 of its first
signing certificate from the APK (`apk.c`: APK Signing Block v2/v3, else
`META-INF/*.RSA`). Then it sends microG's register3 request:
`android.clients.google.com/c2dm/register3`, `Authorization: AidLogin`,
`app`, `device`, `cert`, `app_ver`, `target_ver`, `sender`, and every other
string extra as `X-<key>`.

The answer goes back as a `sendBroadcast` of `c2dm.intent.REGISTRATION` to
the asking connection, with `registration_id` (prefixed with the request's
`kid`, `|ID|n|`, which Firebase matches) or `error`. Registered packages are
listed in `~/.local/state/zrn-pushd/atl-registrations`.

A message for a registered ATL package is a `sendBroadcast` of
`c2dm.intent.RECEIVE` to the app's D-Bus name, which is its package.
`ztos-android` now starts ATL apps with `--gapplication-app-id <package>`,
so they own that name. It also writes
`~/.local/share/dbus-1/services/<package>.service`, so D-Bus starts an app
that is not running (its window opens, as ATL does upstream).
`ztos-android sync` writes the file for apps installed before this change.

When one package is registered in both runtimes, its messages go to the ATL
copy, because the category does not tell the two apart.

## Install

On the phone, as root, from a copy of this directory:

```sh
sudo ./install.sh
```

It builds, installs, writes the sudoers rule, enables the user unit for every
session (`systemctl --global enable`), and restarts it. The OS installer does
the same (`installer/ztos_installer/build.py`). `recover.sh`, `power.sh` and
`ztos-android` are installed with their own components.

## Looking at it

```sh
systemctl --user status zrn-pushd
journalctl --user -u zrn-pushd            # no identity, tokens or message contents in it
cat $XDG_RUNTIME_DIR/zrn-pushd.status     # state, heartbeat, counts
pkill -USR1 zrn-pushd                     # a heartbeat now
pkill -HUP zrn-pushd                      # identity again from microG, reconnect
```

To test an app's side without Google, send it a message made locally. It is
not acknowledged to anyone:

```sh
gdbus call --session --dest com.google.android.gms --object-path /org/ztos/Push \
    --method org.ztos.Push1.Deliver com.example.app waydroid "{'key': 'value'}"
```
