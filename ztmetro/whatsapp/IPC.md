# ztwhatsappd IPC

Version 1. Owned by the engine (`ztmetro/whatsapp/engine/`). The UI and the
notification agent are clients. New fields may be added at any time, so clients
must ignore fields they do not know. Removing or changing a field raises
`ipc_version`.

## Transport

- Unix stream socket at `$XDG_RUNTIME_DIR/ztwhatsapp.sock`, mode 0600.
- Newline-delimited JSON, UTF-8, one object per line. Longest line 4 MiB.
- Any number of clients may connect. Every client receives every event.
- Binary: `/usr/bin/ztwhatsappd`. Only one instance runs per user (flock). If
  connecting fails, a client may start it detached and retry for 3 seconds.
  The daemon normally runs as a systemd user service,
  `zrn-ztwhatsappd.service`, so notifications work with the UI closed.
- Clients must block in poll or a socket notifier, never poll on a timer. A
  client that lets 1 MiB of events queue up is disconnected and may reconnect.

## Three kinds of line

```
client -> daemon  {"id":7,"cmd":"chats","limit":50}
daemon -> client  {"id":7,"ok":true,"result":{...}}
daemon -> client  {"id":7,"ok":false,"error":"not_connected","message":"..."}
daemon -> client  {"event":"message", ...}
```

`id` is any integer chosen by the client and is echoed back. Responses and events
may interleave. On connect the daemon sends a `hello` event, then `connection` and
`pairing` events with the current state, so a client needs no separate query.

Error codes: `bad_request`, `unknown_cmd`, `not_paired`, `not_connected`,
`not_found`, `no_session`, `send_failed`, `timeout`, `busy`, `unsupported`,
`internal`.

## Identifiers

- Chat ids are WhatsApp JIDs as strings: `61400000000@s.whatsapp.net` for a person,
  `1203630000000@g.us` for a group, `12345@lid` for a person known only by a
  privacy id. Treat them as opaque.
- Message ids are opaque strings, unique within a chat.
- All timestamps are Unix seconds, as integers.

## Objects

Chat:

```json
{"id":"...@s.whatsapp.net","name":"Sam","is_group":false,"unread":2,
 "last_text":"see you","last_ts":1793400000,"last_from_me":false,
 "muted":false,"pinned":false,"archived":false}
```

`name` is never empty: it falls back to the number. `last_text` is a preview, and
media gives a label such as `Photo`.

Message:

```json
{"chat":"...","id":"3EB0...","from_me":false,"sender":"...@s.whatsapp.net",
 "sender_name":"Sam","timestamp":1793400000,"kind":"text","text":"hello",
 "status":"received","quoted_id":"","media":null}
```

- `kind`: `text`, `image`, `video`, `audio`, `document`, `sticker`, `contact`,
  `location`, `reaction`, `system`, `unsupported`, `undecryptable`.
- `text` is the body or caption. For `unsupported` and `undecryptable` it is a short
  placeholder suitable for showing.
- `status`, for own messages: `pending`, `sent`, `delivered`, `read`, `failed`.
  For others: `received`, `read`.
- `media`, when present:
  `{"mime":"image/jpeg","size":12345,"width":800,"height":600,"file_name":"",
  "thumb_b64":"<small JPEG, may be empty>","path":""}`. `path` is filled in once
  downloaded. It is a decrypted file in `~/.local/share/ztwhatsapp/media`, mode 0600.

StatusContact:

```json
{"sender":"...@s.whatsapp.net","name":"Sam","last_ts":1793400000,"unseen":2,"items":[StatusItem]}
```

StatusItem is a Message with `chat` = `status@broadcast`, plus `"seen":bool`. Text, image
and video items all arrive; fetch an image with `download_media` using that chat and the
item's id. Video stays `unsupported` for download, as for chats.

Contact:

```json
{"jid":"...@s.whatsapp.net","name":"Sam Example","push_name":"Sam","phone":"+61400000000"}
```

`name` is the address-book name from app-state sync, falling back to the push name, then the
number. Message gains `"starred":bool`; absent means false.

App-state values (`muted`, `pinned`, `archived`, `starred`, contact names) are **read-only**
here: the phone's changes are followed, and there are no commands that change them.

## Commands

| cmd | fields | result |
|---|---|---|
| `status` | | `{"ipc_version":1,"connection":..,"pairing":..,"self":"jid or empty","push_name":""}` |
| `pair_qr` | | `{}`. Begins QR pairing. Codes arrive in `pairing` events. |
| `pair_phone` | `phone` (digits with country code) | `{"code":"ABCD-EFGH"}`, also in a `pairing` event. |
| `pair_cancel` | | `{}` |
| `logout` | | `{}`. Unlinks this device and wipes the local store. |
| `chats` | `limit` (default 100), `offset` (default 0) | `{"chats":[Chat],"total":N}`, newest first, pinned first |
| `messages` | `chat`, `limit` (default 50), `before_ts` (optional), `before_id` (optional) | `{"messages":[Message],"more":bool}`, **oldest first within the page**. Omit `before_*` for the newest page. To page back, pass the oldest message's `timestamp` and `id`. |
| `send_text` | `chat`, `text` (at most 65536 bytes), `quoted_id` (optional) | `{"id":"...","timestamp":N}`. Returns once stored as `pending`. Progress arrives in `receipt` events. |
| `mark_read` | `chat`, `ids` (optional, default all unread) | `{"marked":N}` |
| `download_media` | `chat`, `message_id` (the top-level `id` is the request id) | `{"path":"..."}`. Replies when finished; other commands keep working meanwhile. Images only at first, others give `unsupported`. |
| `statuses` | | `{"contacts":[StatusContact]}`, newest first. Items older than 24 hours are left out. |
| `status_seen` | `sender`, `ids` | `{}`. Sends seen receipts, unless the account has read receipts off. |
| `post_status` | `text` (at most 700 bytes) | `{"id":"...","timestamp":N}`. Text only. |
| `contacts` | `query` (optional, matches name or number), `limit` (default 200), `offset` | `{"contacts":[Contact],"total":N}`, by name. Use it to start a new chat: pass `jid` as `chat` to `send_text`. |
| `profile_picture` | `jid`, `preview` (bool, default true) | `{"path":"..."}`, a JPEG in `media/`, mode 0600. `not_found` when there is none or privacy hides it. |
| `group_info` | `chat` | `{"id":"","subject":"","topic":"","owner":"","created":N,"announce":bool,"locked":bool,"participants":[{"jid":"","name":"","is_admin":bool,"is_super_admin":bool}]}` |
| `search` | `query`, `chat` (optional), `limit` (default 50), `before_ts` (optional) | `{"messages":[Message],"more":bool}`, newest first, case-insensitive. Runs over decrypted rows, so it may take a moment on a large store. |
| `starred` | `limit` (default 100) | `{"messages":[Message]}`, newest first |
| `typing` | `chat`, `state` (`composing` or `paused`) | `{}` |
| `set_presence` | `available` (bool) | `{}`. The UI sends true when in the foreground, false when hidden. |
| `subscribe_presence` | `chat` | `{}`. Later changes arrive in `presence` events. |

## Events

### `hello`
`{"event":"hello","ipc_version":1,"daemon_pid":123}`

### `connection`
`{"event":"connection","state":"connected","detail":""}`

`state`: `offline` (no network or waiting to retry), `connecting`, `connected`
(authenticated and live), `logged_out` (the phone removed this device, so pair again).

### `pairing`
`{"event":"pairing","state":"qr","qr":"<string to render>","expires_in":20,"code":"","error":""}`

`state`:
- `unpaired`: nothing started. Show the choice of QR or phone number.
- `qr`: render `qr` as a QR code. Replaced by a later event when it rotates.
- `phone_code`: show `code`. The user types it into the phone.
- `pairing`: the phone has responded; the handshake is finishing.
- `paired`: done. `self` holds the account JID.
- `failed`: `error` gives the reason. The state then falls back to `unpaired`.

The `qr` value is the exact text to encode. Clients do their own QR rendering.

### `message`
New message or a change to one. **This is the notification trigger.**

```json
{"event":"message","chat":"...","chat_name":"Family","sender":"...",
 "sender_name":"Sam","text":"hello","timestamp":1793400000,"id":"3EB0...",
 "from_me":false,"is_group":true,"muted":false,"live":true,"message":{Message}}
```

`chat`, `sender_name`, `text` and `timestamp` are duplicated at the top level for
clients that need nothing else. `live` is false for offline backlog, history sync
and anything older than five minutes. **Notify only when `live` is true, `from_me` is false and `muted` is false.**

### `chat`
`{"event":"chat","chat":{Chat}}`. A row was created or changed, or its unread count moved.

### `receipt`
`{"event":"receipt","chat":"...","ids":["..."],"status":"delivered","sender":""}`

`status` as in Message. `failed` includes `"error":"..."`.

### `presence`
`{"event":"presence","chat":"...","available":true,"last_seen":0,"typing":false}`

### `history_sync`
`{"event":"history_sync","state":"running","progress":40,"chats":12,"messages":800}`

`state`: `running` or `done`. The chat list should be reloaded on `done`, or as `chat`
events come in.

### `status`
`{"event":"status","sender":"...","sender_name":"Sam","item":{StatusItem},"live":true}`

A status arrived. Never a notification trigger by itself.

### `contacts`
`{"event":"contacts","total":N}`. The contact list changed: reload it.

### `app_state`
`{"event":"app_state","state":"running"}`, then `"done"`. Chat rows follow as `chat` events.

### `picture`
`{"event":"picture","jid":"..."}`. A contact or group changed picture: fetch again if shown.

### `error`
`{"event":"error","code":"...","message":"..."}`. Daemon-level problems worth a log
line. Never contains message content or keys.

## Calls

Calls are out of scope. The engine has no call events or commands, and it
acknowledges and ignores incoming call stanzas. An incoming call rings only on
the user's other devices.

## Privacy

The daemon logs no message text, contact names, keys or numbers. Clients must keep
to the same rule.

## Trying it

`ztwhatsappd --fake` serves this same socket with a scripted account: fake chats, a
new message every 20 seconds, and a QR pairing flow. It never touches
the network, so UI and notifier work can be tested with it without a real account.
`--socket PATH` overrides the socket path.
