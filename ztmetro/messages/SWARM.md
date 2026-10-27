# Messaging task report

## Changed

- `ztmetro/messages/ztmessages.c`: rebuilt the conversation list and thread view for WP 8.1 style; added unread/read state, select and delete, a fixed compose strip, People suggestions, draft discard, SMS dry-run support, Unicode-safe snippets, and longer message storage.
- `ztmetro/modemd/ztoscorenetworkingd.c` already has SMS multipart/unicode handling and offline encode/inject hooks in this shared worktree. Its SMS-only hooks were used for verification; the daemon binary was not installed or restarted.
- Installed `/usr/local/bin/ztmessages` from an on-phone build. Backup: `/usr/local/bin/ztmessages.before-messages`.

## Verified

- Built `messages/ztmessages` on the phone. The installed binary’s SHA-256 matches the build: `9a0ee404750446704bbc348f33fdbc1c48537f5eeb37bb66894926d91b430e05`.
- Ran installed `--dry-run +94770000000 "test path"`; it returned `Test send: 9 bytes; modem untouched.` No real SMS was sent.
- Verified fake incoming Unicode SMS, banner notification, and live update of the matching open thread. Evidence: [notification](evidence/live/messages-notification-live.png), [open thread update](evidence/live/messages-received-live.png).
- Verified the SMS codec with ASCII, GSM multipart, Unicode, UTF-16 multipart, maximum supported ASCII/Unicode lengths, 16-bit concatenation references, invalid input rejection, and restart between parts. Results: [SMS multipart verification](evidence/live/sms-multipart-verification.json).
- Captured the views on the phone: [threads](evidence/live/messages-threads-live.png), [thread](evidence/live/messages-thread-live.png), [new message](evidence/live/messages-new-live.png), [People suggestions](evidence/live/messages-people-live.png), [keyboard](evidence/live/messages-keyboard-live.png), [typed draft](evidence/live/messages-typed-live.png), [dry-run send](evidence/live/messages-dry-send-live.png), [select mode](evidence/live/messages-select-live.png), [selected thread](evidence/live/messages-selected-live.png).
- `git diff --check` passed for the messaging UI and SMS daemon source.

## Still limited

- Attachments need MMS support; the app explains this in the compose menu.
- The phone’s shared screen was in use by another task after capture, so I did not replace or relaunch the frontmost app. The installed binary is ready on next launch.

## Polish follow-up, 2026-10-25

The compose renderer now clears Cairo's current path after placeholder/draft
text and between the round send/attach buttons. A text current point must not
be stroked with round line caps by a later shape. The shared core already has
a similar guard inside its round-button renderer; the compose strip now owns
its path boundaries explicitly too.

Built and installed from the isolated on-phone `~/swarm-polish` tree, after
backing up `/usr/local/bin/ztmessages.before-polish`. Inspected the original
`evidence/live/messages-thread-live.png`: it shows the three reported path
artifacts. Fresh `evidence/live/polish-compose-after.png` shows a clean
placeholder and clean send/attach circles. The installed predecessor already
benefited from the core round-button path fix; the explicit compose guards
are now deployed too. No message was sent or user conversation data changed.

Typing and the keyboard were exercised too. Some later incremental grim
captures omitted static shell/status/button pixels; those partial captures
are not full-layout proof. The complete empty-compose screenshot above is
the decisive visual check. Test text was discarded; no SMS was sent.
