# zrn-passcoded: the lock screen's passcode, in a LUKS2 vault

The passcode is the passphrase of a LUKS2 keyslot. Checking a passcode means opening that keyslot with Argon2id. No hash of the passcode is stored anywhere. The vault will later hold the fingerprint and iris templates and other secrets.

**It is off by default.** Installing it changes nothing. The lock screen asks for a passcode only after the owner sets one in Settings > passcode. The shell falls back to a swipe whenever the daemon is down, crashed or slow to answer (see `LOCKSCREEN-SPEC.md`).

## Files

| Path | What | Mode |
|---|---|---|
| `/usr/local/sbin/zrn-passcoded` | the daemon (root) | 755 |
| `/usr/local/sbin/zrn-passcode` | the CLI for tests and recovery (root only) | 755 |
| `/etc/systemd/system/zrn-passcoded.service` | the unit | 644 |
| `/run/zrn-passcoded.sock` | the API socket, `root:<user group>` | 0660 |
| `/var/lib/zrn-passcode/` | the state directory | 0700 root |
| `.../vault.img` | the LUKS2 vault, 8 MiB | 0600 |
| `.../state` | enabled, kind, length, wrong-attempt count, wait; no secrets | 0600 |
| `.../recovery.key` | the recovery key, only if the owner chose to keep it | 0400 |
| `.../vault-<time>.img`, `recovery-<time>.key` | an old vault put aside by "start a new vault"; never deleted | 0600 |

## The vault

- **Header:** LUKS2, `aes-xts-plain64`, a 512-bit volume key and 512-byte sectors. Its JSON area is 16 KiB and its keyslot area 2 MiB, with room for 8 slots. The data starts at 3 MiB, leaving 5 MiB of data. The label is `zrn-passcode`.
- **Keyslot 0 holds the passcode.** It uses Argon2id with **64 MiB of memory, 4 passes and 4 lanes**. The parameters are fixed (`CRYPT_PBKDF_NO_BENCHMARK`), so they are the same on every phone. You can override them with `--argon-kb`, `--argon-iter` and `--argon-threads`.
- **Keyslot 1 holds the recovery key:** 256 random bits, written as 52 Crockford base32 characters in 13 groups of 4. It uses PBKDF2-SHA256 at 1000 iterations, which is enough for a key with this much entropy.
- **The data area holds a small archive, not a filesystem.** The S9 kernel has `CONFIG_MD` off, so it has no device-mapper and no dm-crypt. The vault is therefore never mapped. libcryptsetup opens the keyslots in user space, and the daemon encrypts the data area itself with AES-256-XTS and plain64 IVs. These are the same bytes dm-crypt writes, so on a kernel with dm-crypt, `cryptsetup open vault.img` shows the same plaintext.
- **Archive format:** `"ZRNVLT01"`, then u32 body length, u32 entry count, the SHA-256 of the body, and then the entries. Each entry is `u16 name length, u32 data length, name, data`, little endian. A name that ends in `/` is a directory. The archive currently holds:
  - `passcode.meta` (`format, kind, length, set_at, changed_at`)
  - `biometrics/`
  - `secrets/`
- **Writes are crash-safe.** Each change is made on `vault.img.new`, which is fsynced and then renamed over the old vault. A crash therefore leaves either the old vault or the new one, never a half-written one.

### Argon2id parameters as measured on the S9

`cryptsetup open --test-passphrase` was run on a test vault, and the daemon's `verify` was run through the CLI:

| Memory, passes, lanes | One check | Conditions |
|---|---|---|
| 64 MiB, 4, 4 (**chosen**) | **0.50–0.54 s** | daemon, first run |
| 64 MiB, 4, 4 | 1.7–2.8 s | daemon, screen off: zrnperfd caps the little cores at 1.15 GHz and the big cores at 0.4 GHz, with 3 cores online |
| 64 MiB, 4, 4 | 2.1–2.4 s | cryptsetup CLI (with process start-up), screen off |
| 128 MiB, 4, 4 | 1.1–4.2 s | cryptsetup CLI |
| 128 MiB, 6–8 passes | 6.5–13 s | cryptsetup CLI, swapping |
| 256 MiB, 4–8 passes | 9–19 s | cryptsetup CLI, swapping |

About 1 GB of memory is free while Waydroid runs, and swap is in use. At 128 MiB or more, a check pushes the phone into swap and takes many seconds. At 64 MiB, a check stays near 0.5 s when the CPU is not capped. The lock screen will check the passcode with the screen on, so it runs near the uncapped figure. The daemon's peak memory was 88 MiB. The unit sets `MemoryMax=256M` and `MemorySwapMax=0`, so key material is never swapped.

## The API

The API runs over a Unix `SOCK_SEQPACKET` socket at `/run/zrn-passcoded.sock`. Each request uses one connection, one request packet and one reply packet. A request is `COMMAND\nkey=value\n...`, and a reply is `ok\nkey=value...` or `error\ncode=CODE\nmessage=...\n...`. `passcode-proto.h` holds a client (`pc_request`, `pc_build`, `pc_get`) with its timeouts.

**Who may call:** `SO_PEERCRED` must show either root or the phone's user (from `/etc/ztos/user.conf`, or else UID 1000). A user process must also run `/usr/local/bin/ztmetro` or `/usr/local/bin/ztsettings`, checked through `/proc/PID/exe`. Everything else gets `code=denied` and a log line.

| Command | Fields | Reply | Who |
|---|---|---|---|
| `status` | | `enabled kind length set_at vault recovery_file failures locked_until now version` (answered at once, never behind a check) | shell, Settings, root |
| `set` | `kind=pin\|alnum new=` [`unlock=` old passcode or recovery key] [`archive=1`] [`new_recovery=1`] | `ok` [`recovery_key=` once] | shell, Settings, root |
| `verify` | `pass=` | `ok` or `wrong`/`locked` with `failures locked_until now` | shell, Settings, root |
| `change` | `old= new=` [`kind=`] | `ok` | shell, Settings, root |
| `disable` | `pass=` | `ok`: the requirement goes, the vault stays | shell, Settings, root |
| `save-recovery` | `key=` | `ok`: writes `recovery.key` (0400) after checking that the key opens slot 1 | shell, Settings, root |
| `inspect` | `pass=` (passcode or recovery key) | the archive's entries and `meta.*` | root |
| `recover` | | `ok vault=1`: clears the requirement and the lockout, keeps the vault | root |

**Error codes:**

| Code | Meaning |
|---|---|
| `wrong` | the passcode was wrong |
| `locked` | wait until `locked_until` |
| `not-set` | no passcode is set |
| `already-set` | a passcode is already set; change it instead |
| `vault-exists` | an earlier vault exists: send `unlock=` or `archive=1` |
| `bad-passcode` | PIN: 4–16 digits; password: 4–64 printable ASCII characters |
| `bad-request`, `denied`, `busy`, `internal` | |

**Turning it on again over an existing vault.** `set` with `unlock=` reuses the vault and its data. It writes slot 0 again and keeps the recovery key unless `new_recovery=1` is sent. With `archive=1`, the old vault is renamed `vault-<time>.img` and a new vault is made.

## Brute force

- **The schedule:** five wrong attempts are free. After that, the waits are 30 s, then 1 min, 5 min, 15 min, and 1 h for each further wrong attempt. A right passcode resets the count.
- **The count survives restarts:** it lives in `state` and is written (fsync and rename) **before** Argon2 runs. A restart, a crash or a power cut during a check does not give a free attempt.
- **Clock changes:** if the clock moves back past the last attempt, the remaining wait is kept relative to the new time, so the wait neither expires nor becomes decades long.
- **Errors that are not the owner's** (the vault cannot be read) give the attempt back.
- **Nothing is ever wiped automatically.**

## Recovery (root over SSH)

```sh
ssh zerone@<phone>
sudo zrn-passcode recover
```

This clears the passcode requirement and the lockout. The phone then unlocks with a swipe. **The vault is kept.** Its data opens with the recovery key:

- from the CLI: `zrn-passcode set pin --unlock` with the new PIN, then the recovery key, on stdin;
- or from Settings: turning the passcode on asks for the old passcode or the recovery key.

If the daemon is down, `recover` edits `state` itself (offline) and tells you to restart the daemon. If the owner chose to keep a copy, the recovery key is in `/var/lib/zrn-passcode/recovery.key`, readable by root only.

**Other CLI commands:** `zrn-passcode status | verify | set | change | disable | save-recovery | inspect`. Secrets are read from stdin, one per line, and never from argv. `ZRN_PASSCODE_SOCKET` points the CLI at a test daemon.

## Safety

- **Off by default.** With no state file, `enabled=0`.
- **Fail open:**
  - If the state says the passcode is on but the vault is missing, the daemon turns the requirement off and logs an error, because nothing could ever unlock it.
  - If the state file is unreadable, the daemon reads it as "off".
  - The shell falls back to a swipe whenever the daemon does not answer (see `LOCKSCREEN-SPEC.md`).
- **Nothing outside the vault is touched:** root, home, fstab, crypttab and the initramfs stay as they are.

## Tests

`sudo ./test-vault.sh [BUILD_DIR]` runs the full test against a test daemon in a transient unit (`zrn-pc-test`), on a test vault in `/run/zrn-pc-test`. It covers:

- set, and verify with the right and wrong passcode;
- the lockout after 6 wrong attempts, and the lockout surviving a daemon restart;
- change, and the recovery key (opening the vault, save-recovery, the file mode);
- disable, then set again with the recovery key;
- `SO_PEERCRED` refusing other programs;
- recover, both over the socket and offline.

It removes everything at the end. Its secrets are made at random and never printed. On 21 Nov 2026 it passed 47 of 47 checks.

## Known limitations

- **A short PIN can be brute-forced offline.** Anyone with root, or with an image of the storage, can copy `vault.img` and try all 10^6 six-digit PINs at their own speed. At 64 MiB of Argon2id that is slow but finite: on a GPU or CPU farm, a 4–6 digit PIN falls within hours to days. The escalating waits only protect the phone in the hand. **The fix for later:** bind a second factor to the secure world, a key sealed by the TEE or keystore that the vault key is wrapped with, so an offline copy of `vault.img` is useless. The fingerprint work will show what the S9's secure world (Kinibi/TEEGRIS) offers to ztOS.
- **The caller check is uid-based.** The exe allowlist is defence in depth only. Any process of the user can `LD_PRELOAD` into or ptrace ztsettings (the user owns those processes). The real boundary is root against the user. Reading the vault data needs the passcode in any case.
- **No dm-crypt in the S9 kernel**, because `CONFIG_MD` is off. The data area is therefore an archive the daemon reads and writes, not a mounted filesystem. With dm-crypt in a later kernel, the same vault could hold ext4. That needs a migration, because the current data is the archive. No kernel was flashed for this.
- **The PIN length shows in `status`,** as on most phones, so the lock screen can draw the dots. The length is in `state`, outside the encryption.
- **Each check costs about 0.5 s** (2+ s with the screen-off CPU caps), and checks run one at a time.
