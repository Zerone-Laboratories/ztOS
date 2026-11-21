# Licence notes

All C++ under `src/` and `tests/` is written for ztOS. No third-party source file was
copied into it, and nothing here links to the projects in the first table.

## Read as protocol references

| Project | Licence | Used for |
|---|---|---|
| whatsmeow, `go.mau.fi/whatsmeow` (commit 33cfac511629) | MPL-2.0 | Stanza shapes, pairing steps, field numbers, and the order of operations |
| go-libsignal, `go.mau.fi/libsignal` v0.2.2 | GPL-3.0 | Checking wire details of Signal as WhatsApp uses it |
| The Signal specifications: X3DH, Double Ratchet, XEdDSA | Public documents | The algorithms |
| RFC 5869, RFC 4231, RFC 7748, RFC 6455, NIST SP 800-38A and GCM test vectors | Public documents | Primitives and test vectors |

Reading a GPL project does not make this code a derivative of it, but go-libsignal was
read for wire facts and the code was written from the specifications. **This is an
engineering record, not legal advice.** Someone should check it before ztOS distributes
the daemon widely.

`libsignal-protocol-c` was deliberately not used, because it is GPL-3.0 and would bind the
daemon to that licence.

## Protocol data taken as facts

- `src/tokens.inc` is WhatsApp's wire dictionary, generated from whatsmeow's
  `binary/token/token.go`. The daemon cannot interoperate without these exact strings.
- The pinned noise root key, the web client version and protobuf field numbers come from
  the same source. They describe WhatsApp's protocol.

MPL-2.0 is file-level copyleft. `tokens.inc` is the one file generated from an MPL file, so
treat it as carrying MPL-2.0 unless legal advice says otherwise.

## Test-only tool

`tests/gofix/` links whatsmeow (MPL-2.0) and go-libsignal (GPL-3.0) to produce
`tests/fixtures/wire.txt` and to act as an interop peer. It is built only by a developer
who asks for it. It is not part of the daemon, the install target or the device image, and
it must stay that way. Its generated fixtures are test data.

## Runtime dependencies

libsodium (ISC), OpenSSL (Apache-2.0), SQLite (public domain), zlib (zlib), libcurl
(curl licence). nlohmann-json (MIT) is needed at build time only.

## Unofficial client

This is an unofficial WhatsApp client. WhatsApp's terms do not permit that, and it can get
a number banned. Users should link it knowing this.
