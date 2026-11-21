# ztwhatsappd

A headless C++20 daemon that speaks WhatsApp's linked-device protocol natively. The UI and
notifier talk to it over the socket described in `../IPC.md`. It uses no browser, Node or Go.

**Warning: an unofficial client can get a phone number banned.** See `NOTICE.md`.

## Build and test

```
pacman -S libsodium openssl sqlite zlib curl nlohmann-json
make
make test                      # no network
make test GOFIX=/path/to/gofix # adds live interop with go-libsignal
```

Build the optional interop peer with `cd tests/gofix && go build -o /path/to/gofix .`. It is
for development machines only and is never installed.

## Run

```
ztwhatsappd                    # the real engine
ztwhatsappd --fake             # scripted data, no network or disk, for UI work
ztwhatsappd --probe-handshake  # see below
```

An unpaired daemon stays off the network until a client sends `pair_qr` or `pair_phone`.

`--probe-handshake` creates a throwaway identity in a temporary directory, connects up to
the point where the server offers QR refs, prints only a count, and deletes the identity. It
links nothing and involves no account. Use it to check that a release still gets past the
server, for example after a WhatsApp version change.

## Layout

| File | Purpose |
|---|---|
| `src/crypto` | Primitives, XEdDSA |
| `src/node`, `src/tokens.inc` | Binary XML codec, JIDs |
| `src/noise` | Noise XX, certificate chain check |
| `src/sigproto` | X3DH, Double Ratchet, sender keys |
| `src/pb.h`, `src/wamsg` | Small protobuf reader and writer, Message parsing |
| `src/ws`, `src/loop.h` | TLS WebSocket, epoll loop |
| `src/store` | Encrypted SQLite store |
| `src/ipc`, `src/fake` | Socket server, scripted engine |
| `src/client` | Connection, pairing, send and receive |

## Idle behaviour

One thread sleeps in `epoll_wait`. It wakes for socket data and for the keepalive every 20
to 30 seconds, which the protocol needs while linked. There is no timer otherwise. While the
link is down, retries back off to about two minutes.

## Storage

`~/.local/share/ztwhatsapp`, mode 0700: `store.db` (SQLite, WAL), `store.key` (0600) and
`media/` (decrypted images, 0600).

Every value is sealed with XChaCha20-Poly1305, and row keys and chat ids are keyed hashes.
Message timestamps, unread flags, row sizes and row counts stay in the clear.

**What this protects:** a copy of the database alone, a backup that leaves out the key, and
casual inspection. **What it does not protect:** anyone who can read `store.key`, meaning
this user's processes or an unencrypted disk image. The key sits beside the database
because the daemon must start unattended. Downloaded media in `media/` is plain files.

## Version

The server rejects stale clients with login failure 405. `client.cpp` holds the version
whatsmeow shipped. To update without a rebuild, put `a.b.c` in
`~/.local/share/ztwhatsapp/wa-version`. The daemon reports 405 in the `connection` event's
`detail` and stops retrying.

## Not implemented

- Calls: stanzas are acknowledged and ignored, by decision.
- Changing app state from here (mute, pin, archive, star, contact edits): it is read-only.
  Missing app-state keys are not requested from the phone; the daemon waits for its next share.
- Sending media. Downloading anything other than images.
- Newsletters and broadcast lists: acknowledged and dropped. Status posts are text only.
- `cstoken` for first contact. Only `tctoken` is attached, so a first message to someone
  who has never messaged this account may be refused. This has not been tried live.
- Identity pinning: a contact's changed identity key is accepted, which is also whatsmeow's
  default.
