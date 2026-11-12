#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""zrn-contacts-sync: Google contacts into the ztOS contact store, one way.

    zrn-contacts-sync signin [--paste] [--browser] [--port N]
    zrn-contacts-sync once
    zrn-contacts-sync status
    zrn-contacts-sync signout

See contacts/CONTACTS-SYNC.md. Standard library only: it runs from a timer on
the efficiency cores, and it exits between runs.
"""
import argparse
import base64
import fcntl
import hashlib
import html
import http.server
import json
import os
import secrets
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

AUTH_URL = "https://accounts.google.com/o/oauth2/v2/auth"
TOKEN_URL = "https://oauth2.googleapis.com/token"
REVOKE_URL = "https://oauth2.googleapis.com/revoke"
PEOPLE_URL = "https://people.googleapis.com/v1/people/me/connections"
SCOPE = "https://www.googleapis.com/auth/contacts.readonly"
# what sync tokens from before contact photos were asked for with: a token only
# answers the same question, so the first sync after the change takes one last
# answer with these, then starts over with PERSON_FIELDS
OLD_PERSON_FIELDS = "names,phoneNumbers,emailAddresses,biographies,organizations,metadata"
PERSON_FIELDS = OLD_PERSON_FIELDS + ",photos"
PHOTOS_VERSION = 1
PHOTO_PX = 192		# tile() in ztcontacts.c is 48 points at 4 pixels to the point
PHOTO_MAX_BYTES = 4 * 1024 * 1024
PHOTO_GIVE_UP = 5	# failures in a row: the network is away, try again next run
DEFAULT_PORT = 8765
UID_PREFIX = "g:"
# struct contact in comm/comm.h: keep these and MAX_CONTACTS in step with it
LIMITS = {"name": 63, "mobile": 31, "other": 31, "email": 63, "note": 95, "uid": 63}
COLUMNS = ("name", "mobile", "other", "email", "note", "uid")
MAX_CONTACTS = 2000


class SyncError(Exception):
    pass


class NeedsSignIn(SyncError):
    pass


class HttpFailure(SyncError):
    def __init__(self, status, body):
        super().__init__(f"HTTP {status}")
        self.status = status
        self.body = body


def config_dir():
    root = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    return os.path.join(root, "ztos", "google-contacts")


def store_path():
    return os.path.expanduser("~/.local/share/ztmetro/contacts")


def write_private(path, text):
    """Atomically, and never readable by anyone else, even for an instant."""
    os.makedirs(os.path.dirname(path), mode=0o700, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path), prefix=".new-")
    try:
        with os.fdopen(fd, "wb" if isinstance(text, bytes) else "w",
                       **({} if isinstance(text, bytes) else {"encoding": "utf-8"})) as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        os.rename(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise


def read_json(path, default=None):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (FileNotFoundError, ValueError):
        return default


def write_json(path, value):
    write_private(path, json.dumps(value, indent=1) + "\n")


def write_status(state, detail="", **extra):
    """What People shows: key=value, no names or numbers, never a token."""
    lines = {"state": state, "detail": detail, "at": str(int(time.time()))}
    lines.update({k: str(v) for k, v in extra.items()})
    write_private(os.path.join(config_dir(), "status"),
                  "".join(f"{k}={v}\n" for k, v in lines.items()))


# ---- from Google's Person to a line of the store ---------------------------

def clean(text, limit):
    """One field: no tabs or line ends, whole UTF-8 characters, fits comm.h."""
    text = " ".join(str(text or "").split())
    raw = text.encode("utf-8")[:limit]
    return raw.decode("utf-8", "ignore")


def uid_of(resource):
    uid = UID_PREFIX + resource
    if len(uid.encode()) > LIMITS["uid"]:
        uid = UID_PREFIX + hashlib.sha256(resource.encode()).hexdigest()[:40]
    return uid


def first_value(person, key, field):
    for item in person.get(key) or []:
        if item.get(field):
            return item[field]
    return ""


def contact_of(person):
    """The contact a Person becomes, or None if nothing in it can be shown."""
    numbers = []
    for phone in person.get("phoneNumbers") or []:
        number = phone.get("canonicalForm") or phone.get("value")
        if number:
            numbers.append(((phone.get("type") or "").lower(), number))
    # mobile first, whatever order they came in; sort is stable
    numbers.sort(key=lambda n: 0 if "mobile" in n[0] else 1)
    name = (first_value(person, "names", "displayName")
            or first_value(person, "organizations", "name")
            or first_value(person, "emailAddresses", "value")
            or (numbers[0][1] if numbers else ""))
    if not name or not person.get("resourceName"):
        return None
    return {
        "name": clean(name, LIMITS["name"]),
        "mobile": clean(numbers[0][1] if numbers else "", LIMITS["mobile"]),
        "other": clean(numbers[1][1] if len(numbers) > 1 else "", LIMITS["other"]),
        "email": clean(first_value(person, "emailAddresses", "value"), LIMITS["email"]),
        "note": clean(first_value(person, "biographies", "value"), LIMITS["note"]),
        "uid": uid_of(person["resourceName"]),
    }


def digits_tail(number):
    digits = "".join(c for c in number if c.isdigit())
    return digits[-9:]


# ---- the store ---------------------------------------------------------------

class Store:
    """comm.h's file, under the lock ztcontacts takes too."""

    def __init__(self, path=None):
        self.path = path or store_path()
        self.lock_fd = None
        self.rows = []

    def __enter__(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        self.lock_fd = os.open(self.path + ".lock", os.O_CREAT | os.O_RDWR, 0o600)
        fcntl.flock(self.lock_fd, fcntl.LOCK_EX)
        self.rows = []
        try:
            with open(self.path, encoding="utf-8", errors="replace") as f:
                for line in f:
                    cells = line.rstrip("\r\n").split("\t")
                    cells += [""] * (len(COLUMNS) - len(cells))
                    row = dict(zip(COLUMNS, cells))
                    if row["name"]:
                        self.rows.append(row)
        except FileNotFoundError:
            pass
        return self

    def __exit__(self, *exc):
        os.close(self.lock_fd)
        self.lock_fd = None

    def save(self):
        self.rows.sort(key=lambda r: r["name"].lower())
        write_private(self.path, "".join(
            "\t".join(r[c] for c in COLUMNS) + "\n" for r in self.rows))


def merge(rows, changes, deleted, known, full, rebase=False):
    """Apply a sync to the store's rows; returns (rows, known, counts).

    known maps uid to the etag last taken from Google. A person Google has not
    changed since is left alone, so what was edited or deleted on the phone
    stays that way through the full syncs an expired token forces.

    rebase: the etags changed because the question asked of Google did, not the
    people: take the new etag of everyone known and leave their rows alone.
    """
    counts = {"added": 0, "updated": 0, "removed": 0, "dropped": 0}
    by_uid = {r["uid"]: r for r in rows if r["uid"]}
    known = dict(known)
    seen = set()

    for etag, contact in changes:
        uid = contact["uid"]
        seen.add(uid)
        if full and uid in known and known[uid] == etag:
            continue
        if rebase and uid in known:
            known[uid] = etag
            continue
        known[uid] = etag
        if uid in by_uid:
            if {c: by_uid[uid][c] for c in COLUMNS} != contact:
                by_uid[uid].update(contact)
                counts["updated"] += 1
            continue
        # the same person typed in by hand: take it over, do not list them twice
        twin = next((r for r in rows if not r["uid"]
                     and r["name"].lower() == contact["name"].lower()
                     and digits_tail(r["mobile"]) == digits_tail(contact["mobile"])), None)
        if twin:
            twin.update(contact)
            by_uid[uid] = twin
            counts["updated"] += 1
        elif len(rows) >= MAX_CONTACTS:
            known.pop(uid)	# not taken: so the next sync tries again
            counts["dropped"] += 1
        else:
            row = dict(contact)
            rows.append(row)
            by_uid[uid] = row
            counts["added"] += 1

    gone = set(deleted)
    if full:
        gone |= {u for u in by_uid if u not in seen} | {u for u in known if u not in seen}
    for uid in gone:
        known.pop(uid, None)
        if uid in by_uid:
            rows.remove(by_uid[uid])
            counts["removed"] += 1
    return rows, known, counts


# ---- talking to Google -------------------------------------------------------

def http_request(url, data=None, headers=None):
    """(status, body); raises OSError when the network itself failed."""
    body = urllib.parse.urlencode(data).encode() if data is not None else None
    request = urllib.request.Request(url, data=body, headers=headers or {})
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return response.status, response.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")


def http_bytes(url):
    """A picture, whole: (status, bytes). No credentials go with it."""
    request = urllib.request.Request(url)
    with urllib.request.urlopen(request, timeout=20) as response:
        if not response.headers.get("Content-Type", "").startswith("image/"):
            return 415, b""
        body = response.read(PHOTO_MAX_BYTES + 1)
    return (413, b"") if len(body) > PHOTO_MAX_BYTES else (200, body)


class Google:
    def __init__(self, cfg=None, transport=http_request, photo_transport=http_bytes):
        self.dir = cfg or config_dir()
        self.http = transport
        self.photo_http = photo_transport
        self.client = read_json(os.path.join(self.dir, "client.json"))
        self.token = read_json(os.path.join(self.dir, "token.json"))

    def client_fields(self):
        if not self.client or not self.client.get("client_id") \
                or "YOUR-CLIENT-ID" in self.client["client_id"]:
            raise NeedsSignIn(f"no OAuth client: fill in {self.dir}/client.json")
        fields = {"client_id": self.client["client_id"]}
        if self.client.get("client_secret"):
            fields["client_secret"] = self.client["client_secret"]
        return fields

    def store_token(self, reply):
        token = dict(self.token or {})
        token.update(reply)
        token["expires_at"] = int(time.time()) + int(reply.get("expires_in", 3600)) - 60
        self.token = token
        write_json(os.path.join(self.dir, "token.json"), token)

    def refresh(self):
        if not self.token or not self.token.get("refresh_token"):
            raise NeedsSignIn("not signed in")
        fields = self.client_fields()
        fields.update(grant_type="refresh_token", refresh_token=self.token["refresh_token"])
        status, body = self.http(TOKEN_URL, data=fields)
        if status in (400, 401):
            raise NeedsSignIn("Google refused the sign-in: sign in again")
        if status != 200:
            raise HttpFailure(status, body)
        self.store_token(json.loads(body))

    def fetch(self, params):
        if not self.token or time.time() >= self.token.get("expires_at", 0):
            self.refresh()
        url = PEOPLE_URL + "?" + urllib.parse.urlencode(params)
        for attempt in (0, 1):
            auth = {"Authorization": "Bearer " + self.token["access_token"]}
            status, body = self.http(url, headers=auth)
            if status == 401 and attempt == 0:
                self.refresh()
                continue
            break
        if status != 200:
            raise HttpFailure(status, body)
        return json.loads(body)

    def pull(self, sync_token, fields=PERSON_FIELDS):
        """(people, next token, full?). All pages or an exception, never half."""
        params = {"personFields": fields, "pageSize": 1000, "requestSyncToken": "true"}
        if sync_token:
            params["syncToken"] = sync_token
        people, page_token = [], None
        while True:
            query = dict(params)
            if page_token:
                query["pageToken"] = page_token
            try:
                page = self.fetch(query)
            except HttpFailure as e:
                if sync_token and "EXPIRED_SYNC_TOKEN" in e.body:
                    return self.pull(None, fields)	# seven days old: start over
                raise
            people.extend(page.get("connections") or [])
            page_token = page.get("nextPageToken")
            if not page_token:
                return people, page.get("nextSyncToken"), not sync_token


def photo_dir(store_file=None):
    return os.path.join(os.path.dirname(store_file or store_path()), "contact-photos")


def photo_name(uid):
    """FNV-1a, 64 bits: comm/contact-photo.h works out the same name in C."""
    h = 0xcbf29ce484222325
    for byte in uid.encode():
        h = ((h ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return f"{h:016x}.jpg"


def photo_url(url):
    """The address of the same picture at PHOTO_PX: Google's size suffix."""
    head, mark, tail = url.rpartition("=s")
    if mark and tail.split("-")[0].isdigit():
        return f"{head}=s{PHOTO_PX}"
    return f"{url}=s{PHOTO_PX}" if "=" not in url.rsplit("/", 1)[-1] else url


def photo_of(person):
    """The address of a person's own picture, or "" for Google's default one."""
    photos = person.get("photos") or []
    photos = sorted(photos, key=lambda p: 0 if (p.get("metadata") or {}).get("primary") else 1)
    if not photos or photos[0].get("default") or not photos[0].get("url"):
        return ""
    url = photo_url(photos[0]["url"])
    return url if url.startswith("https://") else ""


def remove_quietly(path):
    try:
        os.unlink(path)
    except OSError:
        pass


def sync_photos(google, people, state, store_file, save):
    """Each person's picture, only when their etag says it may have changed.

    Never raises for a picture: what could not be fetched stays in state's
    photo_pending and is tried again next run, since an incremental sync will
    not mention that person again. Picture addresses are not logged anywhere.
    """
    folder = photo_dir(store_file)
    done = state.setdefault("photo_etags", {})
    pending = state.setdefault("photo_pending", {})
    for person in people:
        resource = person.get("resourceName")
        if not resource:
            continue
        uid = uid_of(resource)
        if (person.get("metadata") or {}).get("deleted"):
            pending.pop(uid, None)
            done.pop(uid, None)
            remove_quietly(os.path.join(folder, photo_name(uid)))
            continue
        etag = person.get("etag", "")
        if done.get(uid) != etag or not etag:
            pending[uid] = {"etag": etag, "url": photo_of(person)}
    for uid in [u for u in done if u not in state.get("known", {})]:
        done.pop(uid)
        remove_quietly(os.path.join(folder, photo_name(uid)))
    fetched = failed = in_a_row = since_save = 0
    for uid in list(pending):
        job = pending[uid]
        path = os.path.join(folder, photo_name(uid))
        try:
            if job["url"]:
                status, body = google.photo_http(job["url"])
                if status != 200 or not body:
                    raise SyncError(f"HTTP {status}")
                write_private(path, body)
                fetched += 1
            else:
                remove_quietly(path)
        except Exception:
            # a picture is never worth failing a sync for
            failed += 1
            in_a_row += 1
            if in_a_row >= PHOTO_GIVE_UP:
                break
            continue
        in_a_row = 0
        done[uid] = job["etag"]
        del pending[uid]
        since_save += 1
        if since_save >= 50:
            since_save = 0
            save()	# a stopped run does not start over
    return fetched, len(pending)


def sync_once(google, store_file=None):
    state_path = os.path.join(google.dir, "state.json")
    state = read_json(state_path, {}) or {}
    migrating = state.get("photos_version") != PHOTOS_VERSION
    if migrating and state.get("sync_token"):
        # the last answer to the old question, so that nothing changed on Google
        # since the last sync is lost when the people's etags start over below
        people, _, full = google.pull(state["sync_token"], OLD_PERSON_FIELDS)
        apply_people(people, state, store_file, full, False)
    people, next_token, full = google.pull(None if migrating else state.get("sync_token"))
    counts = apply_people(people, state, store_file, full, migrating and bool(state.get("known")))
    state["sync_token"] = next_token
    state["photos_version"] = PHOTOS_VERSION
    write_json(state_path, state)
    counts["photos"], counts["photos_waiting"] = sync_photos(
        google, people, state, store_file, lambda: write_json(state_path, state))
    write_json(state_path, state)
    return counts


def apply_people(people, state, store_file, full, rebase):
    """What Google said, into the store and state's known; returns the counts."""
    changes, deleted = [], []
    for person in people:
        resource = person.get("resourceName")
        if not resource:
            continue
        if (person.get("metadata") or {}).get("deleted"):
            deleted.append(uid_of(resource))
            continue
        contact = contact_of(person)
        if contact:
            changes.append((person.get("etag", ""), contact))
    known = state.get("known", {})
    # an empty answer to a full sync, with people here: a fault, not a mass delete
    if full and not changes and len(known) > 5:
        raise SyncError("Google returned nobody; the store was left alone")
    with Store(store_file) as store:
        rows, known, counts = merge(store.rows, changes, deleted, known, full, rebase)
        store.rows = rows
        if any(counts[k] for k in ("added", "updated", "removed")):
            store.save()
    state["known"] = known
    counts["total"] = len(known)
    counts["kind"] = "full" if full else "incremental"
    return counts


# ---- signing in: Desktop client, loopback redirect, PKCE ---------------------

def pkce_pair():
    verifier = secrets.token_urlsafe(64)
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest())
    return verifier, challenge.rstrip(b"=").decode()


def auth_url(client_id, redirect, state, challenge):
    return AUTH_URL + "?" + urllib.parse.urlencode({
        "client_id": client_id, "redirect_uri": redirect, "response_type": "code",
        "scope": SCOPE, "access_type": "offline", "prompt": "consent", "state": state,
        "code_challenge": challenge, "code_challenge_method": "S256"})


def code_from_redirect(url, state):
    query = urllib.parse.parse_qs(urllib.parse.urlparse(url.strip()).query)
    if query.get("state", [""])[0] != state:
        raise SyncError("the answer's state does not match this sign-in")
    if query.get("error"):
        raise SyncError("Google said: " + query["error"][0])
    code = query.get("code", [""])[0]
    if not code:
        raise SyncError("no code in that address")
    return code


def wait_for_redirect(port, state, timeout=600):
    """One redirect on 127.0.0.1 only; returns the code."""
    result = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            try:
                result["code"] = code_from_redirect(self.path, state)
                text = "Signed in. You can close this page."
            except SyncError as e:
                # a stray request (a favicon) must not end the sign-in
                if "state" not in urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query):
                    self.send_error(404)
                    return
                result["error"] = str(e)
                text = str(e)
            page = f"<!doctype html><title>ztOS</title><p>{html.escape(text)}</p>".encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(page)))
            self.end_headers()
            self.wfile.write(page)

        def log_message(self, *args):
            pass	# the address holds the code

    server = http.server.HTTPServer(("127.0.0.1", port), Handler)
    server.timeout = 5
    deadline = time.time() + timeout
    try:
        while not result and time.time() < deadline:
            server.handle_request()
    finally:
        server.server_close()
    if "error" in result:
        raise SyncError(result["error"])
    if "code" not in result:
        raise SyncError("nobody signed in within ten minutes")
    return result["code"]


def exchange_code(google, code, verifier, redirect):
    fields = google.client_fields()
    fields.update(grant_type="authorization_code", code=code,
                  code_verifier=verifier, redirect_uri=redirect)
    status, body = google.http(TOKEN_URL, data=fields)
    if status != 200:
        raise HttpFailure(status, body)
    reply = json.loads(body)
    if not reply.get("refresh_token"):
        raise SyncError("Google gave no refresh token")
    google.store_token(reply)


def cmd_signin(args):
    google = Google()
    fields = google.client_fields()
    verifier, challenge = pkce_pair()
    state = secrets.token_urlsafe(24)
    redirect = f"http://127.0.0.1:{args.port}"
    url = auth_url(fields["client_id"], redirect, state, challenge)
    print("Open this address and sign in:\n\n" + url + "\n", flush=True)
    write_status("signing-in", "finish in the browser")
    if args.paste:
        print("When the browser fails to load 127.0.0.1, paste its whole address here:", flush=True)
        code = code_from_redirect(sys.stdin.readline(), state)
    else:
        print(f"From another computer first run: ssh -L {args.port}:127.0.0.1:{args.port} "
              "zerone@172.16.42.1", flush=True)
        if args.browser and shutil.which("superscan"):
            subprocess.Popen(["superscan", url], stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, start_new_session=True)
        code = wait_for_redirect(args.port, state)
    exchange_code(google, code, verifier, redirect)
    print("Signed in. First sync...")
    return cmd_once(args)


def cmd_once(_args):
    try:
        counts = sync_once(Google())
    except NeedsSignIn as e:
        write_status("signed-out", str(e))
        print(e, file=sys.stderr)
        return 0	# not a fault of the timer's
    except (OSError, HttpFailure) as e:
        # offline, or Google busy (429 on full syncs): the next timer tries again
        write_status("retry", str(e).split("\n")[0][:120])
        print("will retry:", e, file=sys.stderr)
        return 0
    except (SyncError, ValueError) as e:
        write_status("error", str(e)[:120])
        print(e, file=sys.stderr)
        return 1
    extra = {k: counts[k] for k in ("total", "added", "updated", "removed", "dropped",
                                    "photos", "photos_waiting")}
    write_status("ok", "the list is full" if counts["dropped"] else counts["kind"], **extra)
    print(" ".join(f"{k}={v}" for k, v in counts.items()))
    return 0


def cmd_status(_args):
    try:
        with open(os.path.join(config_dir(), "status"), encoding="utf-8") as f:
            sys.stdout.write(f.read())
    except FileNotFoundError:
        print("state=never")
    return 0


def cmd_signout(_args):
    google = Google()
    if google.token:
        token = google.token.get("refresh_token") or google.token.get("access_token")
        try:
            google.http(REVOKE_URL, data={"token": token})
        except OSError:
            print("offline: not revoked at Google; removed here", file=sys.stderr)
    # the people stay, as ordinary contacts: signing out must not empty the phone
    with Store() as store:
        for row in store.rows:
            row["uid"] = ""
        if store.rows:
            store.save()
    shutil.rmtree(photo_dir(), ignore_errors=True)	# whose they were is gone with the uids
    for name in ("token.json", "state.json"):
        path = os.path.join(google.dir, name)
        if os.path.exists(path):
            os.unlink(path)
    write_status("signed-out", "")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(prog="zrn-contacts-sync")
    sub = parser.add_subparsers(dest="command", required=True)
    signin = sub.add_parser("signin")
    signin.add_argument("--paste", action="store_true")
    signin.add_argument("--browser", action="store_true")
    signin.add_argument("--port", type=int, default=DEFAULT_PORT)
    sub.add_parser("once")
    sub.add_parser("status")
    sub.add_parser("signout")
    args = parser.parse_args(argv)
    os.umask(0o077)
    try:
        return {"signin": cmd_signin, "once": cmd_once, "status": cmd_status,
                "signout": cmd_signout}[args.command](args)
    except (SyncError, OSError) as e:
        write_status("error", str(e)[:120])
        print(e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
