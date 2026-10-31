# ztwhatsapp-notify

Turns `ztwhatsappd` message events (`../IPC.md`) into freedesktop notifications
for ztnotifyd. Calls are out of scope by the user's decision.

Each statement about the phone is labelled **observed** (read in the code or in
a recorded result) or **inferred**. Nothing here has run on the phone yet.

## What it does

| Need | How |
|---|---|
| App identity and icon | `app_name` WhatsApp, hint `desktop-entry=ztwhatsapp`. ztnotifyd passes it as WHO; the shell finds the glyph by that, then by the name, whose word "whatsapp" gives the message glyph (observed). |
| One row per chat | `replaces_id`, a count and the latest text. Needs `shell-actions.patch`. |
| Tap opens the chat | `default` action. Needs the patch and the UI contract below. |
| None for the chat on screen | The UI's focus file. |
| Sound | The shell's own `ui_sound(UIS_NOTIFY)` for an alerting notification; updates carry `suppress-sound` (observed). One chat alerts at most every 10 s, and one flush alerts once. |
| Read elsewhere | A `chat` event with `unread` 0 closes the row. |

Silent: own messages, muted chats, `system`, `reaction`, repeats of an id, and
anything during a history sync.

**A departure from IPC.md.** It says to notify only when `live` is true, but a
message that arrives during SoC sleep comes as offline backlog, with `live`
false (inferred from the contract's wording), so that rule would silence nearly
every notification on a sleeping phone. This client also takes backlog newer than
a stored watermark, a single number in `~/.local/state/ztwhatsapp/`. Backlog is
compared with the value from the start of the connection, because it comes out of
order between chats.

## Changes needed outside this directory

Nothing below has been applied.

**`shell-actions.patch`, for ztnotifyd and ztmetro.** Today ztnotifyd ignores
`replaces_id`, `CloseNotification` is a no-op, `ActionInvoked` is never sent, and
a shade tap only expands a row (all observed). The patch adds `notifyx`,
`notifyxq` and `notifyclose` lines and `ztnotifyd --invoke ID`. It also stops both
programs writing titles and text to their logs, which they do today (observed)
and which breaks the IPC privacy rule. Apply it with `git apply` from the
repository root.

Without the patch: each alerting message adds a separate row, closing does
nothing, and a tap does not open the chat. Messages are still shown.

**The UI** (`../ui`) needs two small things:

- While a chat is on screen with the screen on, keep `$XDG_RUNTIME_DIR/ztwhatsapp.focus`
  containing `PID CHAT`. Remove it when the chat closes, the app is hidden or the
  screen goes off.
- At start and on change (inotify), read `$XDG_RUNTIME_DIR/ztwhatsapp.open`,
  open the chat it names, and delete it. The notifier writes it, then sends the
  shell `launch ztwhatsapp`. An argument would not reach a running window, because
  the shell raises an existing one by its command (observed).

**The engine** needs to:

- Use no `CLOCK_*_ALARM` timer. zrn-pushd's refused suspends on 29 Oct (observed).
- Notice Wi-Fi leaving and returning (netlink), and reconnect at once rather than
  waiting out a backoff. The window is short.

**zrn-sleepd.** Without this, a phone with no zrn-pushd identity and no background
app sleeps up to 1800 s, not 300 s (observed):

```python
def whatsapp_active():
	return kv(Path(ztos_user.RUNTIME_DIR) / 'ztwhatsapp-notify.status').get('paired') == '1'

# in cycle():
dozing = background_apps_active(fresh=True) or push_active() or whatsapp_active()
```

**The image.** Build and install from this directory with `make install`, then
enable `zrn-ztwhatsapp-notify.service` for the user. The unit expects
`/usr/bin/ztwhatsapp-notify`.

## Staying connected through sleep

**Recommendation: periodic wake, using zrn-sleepd's existing doze windows.** The
notifier adds no wake of its own.

Socket wake does not work on Wi-Fi. zrn-sleepd takes Wi-Fi off its PCIe link for
every sleep because the BCM4361 does not take D3, so the connection is gone and
no packet can wake the SoC (observed). On LTE a connection can survive and a CP
mailbox wake gives 15 s awake (observed in zrn-sleepd), but WhatsApp's keepalive
and server timeout during such a sleep are unmeasured, so LTE is treated the same
way. If its socket does survive, the hold ends early.

One cycle:

1. zrn-sleepd wakes on its RTC alarm, at most 300 s after sleeping, and restores
   Wi-Fi in 4.6 to 6 s (observed).
2. The notifier sees `wakes=` rise in `/run/zrn-sleepd/status` by inotify and takes
   a `Sleep1.Inhibit`.
3. The engine reconnects and delivers its backlog; notifications go out.
4. The notifier releases on the first of: live and quiet for 4 s; 12 s with the
   socket still live and no reconnection; 15 s offline; 25 s in every case.

**It cannot become a veto.** A hold needs a real sleep of at least 60 s, ending in
`rtc`, `alarm`, `other` or `modem`; hub storms and the power key take none. Each
wake has one hold with a hard cap. Sleep1 drops the inhibit when its caller leaves
the bus and it lapses by itself after 300 s (observed). The notifier blocks in
`poll` with no timer running when idle, and its clocks cannot wake the SoC.

**The cost in latency:** a message can wait up to about 5 minutes on a sleeping
phone. There is no FCM route for a linked device here (inferred).

## Battery estimate

All **inferred**. The 133 mA sleep figure is the user's; the awake figure of 400 mA
with Wi-Fi is an assumption, not a measurement. Measure it with `zrn-sleepd log`,
whose lines carry charge in and out.

| Case | Awake per cycle | Average added |
|---|---|---|
| Doze already running for zrn-pushd, hold adds 2 s | 2 s of about 318 s | about 1.7 mA |
| Same, hold reaches its 25 s cap, adds 9 s | 9 s of about 325 s | about 7.4 mA |
| No doze before, this turns it on | 25 s of about 325 s | about 20 mA (13 at 300 mA awake, 28 at 500 mA) |

On a 3000 mAh S9, 133 mA is about 22.5 h; 153 mA is about 19.5 h. The saving over
keeping the veto (`keep_awake`) is far larger than any of these (inferred).

## Tests

```
make check
```

`tests/policy-test.cpp` covers the rules and the hold under ASan and UBSan.
`tests/e2e.py` runs the real binary and a real ztnotifyd (patched if needed) on a
private bus, with a scripted engine and invented data. It does not cover the shell,
the real engine, or a real sleep.

## Known limits

- Android WhatsApp in Waydroid, if it stays set up, also notifies, so each message
  shows twice (inferred).
- A banner tap on the lock screen only puts it away, and the lock screen's rows have
  no tap, so opening a chat from there is not possible.
- CLEAR ALL in the action centre does not tell the notifier: the next message starts
  a new row and keeps counting from before.
