# Google contacts on ztOS

`zrn-contacts-sync` copies Google contacts into the ztOS contact store, one way,
every half hour while the phone is awake. Nothing is ever written to Google.

## Design

| Decision | Why |
| --- | --- |
| Google People API, `contacts.readonly` | Sync tokens report changes and deletions directly; CardDAV would need its own XML and vCard parsing. |
| Desktop OAuth client, loopback redirect, PKCE | Google's device flow allows only sign-in, `drive.appdata`, `drive.file` and YouTube scopes. Contacts and CardDAV are not on that list. |
| Waydroid is not a source | Its contacts exist only when Android is signed in and running; reading its database would tie People to the container. |
| One file, the existing store | Phone and Messaging already read it, and need no new API. |

## The store

`~/.local/share/ztmetro/contacts`, mode 600, one person per line, tab-separated:

    name  mobile  other  email  note  uid

`uid` is `g:<resourceName>` for a Google person, empty for one typed in here.
Limits are in `comm/comm.h`: 2000 people, and the field sizes in `struct contact`.
A person's third and later numbers do not fit and are not kept.

Writers take `flock` on `contacts.lock`, re-read the file, change what they mean
to change, and rename a new file into place (`comm/contact-store.h`). Readers need
no lock.

## What happens when

- **Changed on Google:** Google's version replaces the one here, including edits made here.
- **Deleted on Google:** removed here.
- **Edited or deleted here:** stays so until that person changes on Google. The etag
  of each person is kept, so a full sync does not undo it.
- **Sync token expired (7 days):** one full sync, then incremental again.
- **Offline or Google busy:** nothing changes; the next timer run tries again.
- **Full sync returns nobody while people are known:** refused, store untouched.
- **Signing out:** the token is revoked and removed; the people stay as local contacts.

## Photos

Each person's photo goes to `~/.local/share/ztmetro/contact-photos/<hash>.jpg`,
directory 700, files 600. The hash is FNV-1a 64 of the uid (`photo_name` in the sync,
`comm/contact-photo.h` in C), so the store keeps its six columns.

- Only pictures Google does not mark `default`, at 192 px (the `=s192` suffix).
- Fetched again only when the person's etag changes (`photo_etags` in `state.json`).
- A failed download never fails the sync. It stays in `photo_pending` and is tried again
  on the next run; five failures in a row end that run's attempts.
- The first sync with this version asks Google once with the old fields, to catch up, then
  once in full with photos (`photos_version`). Etags can change with the fields asked for,
  so that one sync only takes the new etags of known people and keeps edits made here.
- Signing out removes the folder.

## Files

In `~/.config/ztos/google-contacts/`, directory 700, files 600:
`client.json` (the owner's OAuth client), `token.json`, `state.json` (sync token
and etags), `status` (what People shows). None of them belongs in a repo.

## Setup

1. In https://console.cloud.google.com create a project, and enable **People API**.
2. Under **Google Auth Platform**, set up the consent screen (External), add
   `.../auth/contacts.readonly`, add yourself as a test user, then **Publish** it.
   Refresh tokens of an app left in Testing stop working after about 7 days.
3. Create an OAuth client of type **Desktop app**.
4. On the phone:

       mkdir -p -m 700 ~/.config/ztos/google-contacts
       install -m 600 /usr/local/share/ztos/google-client.json.template \
           ~/.config/ztos/google-contacts/client.json
       nano ~/.config/ztos/google-contacts/client.json   # client_id, client_secret

5. Sign in. Any one of:
   - People, **google**, **sign in** (opens Superscan on the phone).
   - Over USB, from a computer:
     `ssh -L 8765:127.0.0.1:8765 zerone@172.16.42.1 zrn-contacts-sync signin`,
     then open the printed address on that computer.
   - `zrn-contacts-sync signin --paste`, sign in on any device, and paste back the
     address the browser fails to load.
6. `systemctl --user enable --now zrn-contacts-sync.timer`

## Tests

`python3 contacts/tests/test_sync.py`: a fake Google and invented people, no network.
