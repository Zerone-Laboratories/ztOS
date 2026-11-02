#!/usr/bin/env python3
"""ZroovMusic torrent source, zroov-source-torrent in the engine's terms (ENGINE.md).

Search only talks HTTP to indexers. Joining a swarm happens only on an explicit
verify or resolve, only after the disclosure was accepted, and through a helper
process that lives no longer than that verify or play. Nothing here stays running.
"""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import signal
import threading
import time
import unicodedata
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

CONSENT_VERSION = 1
DISCLOSURE = (
    "Torrent streaming shares what you download. While a song plays, your phone "
    "uploads parts of it to other people, and everyone in that swarm, and its "
    "trackers, can see your IP address and what you are fetching. You are "
    "responsible for what you choose to play, so only play material you have the "
    "right to use. By default this runs on Wi-Fi only, upload is capped, and "
    "sharing stops when the song stops."
)
DEFAULTS = {
    "torznab_urls": [],  # the user's own indexers; empty by default, none suggested
    "internet_archive": True,
    "ia_collections": ["netlabels", "etree"],
    "wifi_only": True,
    "upload_kib": 64,  # 0 means the lowest libtorrent allows, 1 KiB/s
    "download_kib": 0,
    "seed_after_s": 0,
    "cache_mib": 1024,
    "buffer_seconds": 5,
    "idle_stop_s": 30,
    "ahead_low_s": 20,  # fetched audio beyond what the player has read: refill below this
    "ahead_high_s": 50,  # and up to this
    "dht_for_magnets": False,
}
UA = "ZroovMusic/1 (ztOS)"
MAX_XML = 8 << 20
MAX_TORRENT = 16 << 20
HEAD_TIMEOUT_S = 90
VERIFIED_NOTE = (
    "Read from the FLAC header in hash-checked pieces. The header proves the "
    "stored format, not that the source was really recorded at this resolution."
)


# --- pure helpers, covered by tests ------------------------------------------
def parse_flac_header(data):
    """STREAMINFO from the start of a FLAC file. None when it is not FLAC."""
    i = 0
    if data[:3] == b"ID3" and len(data) >= 10:
        i = 10 + ((data[6] & 127) << 21 | (data[7] & 127) << 14 | (data[8] & 127) << 7 | (data[9] & 127))
    if data[i : i + 4] != b"fLaC" or len(data) < i + 42:
        return None
    if data[i + 4] & 127 != 0:  # the first block must be STREAMINFO
        return None
    s = data[i + 8 : i + 42]
    rate = (s[10] << 12) | (s[11] << 4) | (s[12] >> 4)
    channels = ((s[12] >> 1) & 7) + 1
    bits = (((s[12] & 1) << 4) | (s[13] >> 4)) + 1
    samples = ((s[13] & 15) << 32) | int.from_bytes(s[14:18], "big")
    if rate == 0:
        return None
    return {
        "bit_depth": bits,
        "sample_rate": rate,
        "channels": channels,
        "samples": samples,
        "duration": round(samples / rate, 2) if samples else None,
    }


def tokens(s):
    s = unicodedata.normalize("NFKD", s or "").encode("ascii", "ignore").decode().lower()
    s = re.sub(r"([a-z])([0-9])|([0-9])([a-z])", r"\1\3 \2\4", s)
    return [t for t in re.split(r"[^a-z0-9]+", s) if t and t not in ("the", "a", "an", "and")]


def _cover(want, have):
    want = set(tokens(want))
    if not want:
        return None
    joined = "".join(tokens(have))
    have = set(tokens(have))
    # file names often run words together: BrickCityLoveSong
    return sum(1 for t in want if t in have or (len(t) > 2 and t in joined)) / len(want)


def match_score(q, artist="", title="", path="", duration=None, is_flac=True):
    """0 to 1, with the reasons kept so a caller can show them."""
    text = " ".join((artist or "", title or "", path or ""))
    a = _cover(q.get("artist"), artist + " " + path if artist else text)
    t = _cover(q.get("title"), (title or "") + " " + path)
    want = q.get("duration")
    d = None
    if want and duration:
        gap = abs(float(want) - float(duration))
        d = 1.0 if gap <= 3 else 0.0 if gap >= 20 else 1 - (gap - 3) / 17
    parts = [(0.35, a), (0.45, t), (0.2, d)]
    known = sum(w for w, v in parts if v is not None)
    score = sum(w * v for w, v in parts if v is not None) / known if known else 0
    if t is not None and t < 0.5:
        score = min(score, 0.4)
    if d == 0.0:
        score = min(score, 0.4)
    if not is_flac:
        score = 0
    return round(score, 3), {"artist": a, "title": t, "duration": d, "format": is_flac}


def claimed_from_name(name):
    """What a name says. Never trusted as fact, only reported as a claim."""
    n = (name or "").lower()
    out = {}
    m = re.search(r"\b(16|24|32)\s*[- ]?bit\b", n) or re.search(r"\b(16|24|32)\s*[/-]\s*(44|48|88|96|176|192)", n)
    if m:
        out["bit_depth"] = int(m.group(1))
    m = re.search(r"\b(44[.,]1|48|88[.,]2|96|176[.,]4|192)\s*k?hz\b", n) or re.search(
        r"\b(?:16|24|32)\s*[/-]\s*(44[.,]1|44|48|88[.,]2|88|96|176[.,]4|176|192)\b", n
    )
    if m:
        v = m.group(1).replace(",", ".")
        v = {"44": "44.1", "88": "88.2", "176": "176.4"}.get(v, v)
        out["sample_rate"] = int(float(v) * 1000)
    return out


def is_hi_res(q):
    return bool(q) and ((q.get("bit_depth") or 0) > 16 or (q.get("sample_rate") or 0) > 48000)


def rank_key(o):
    """Header facts outrank any name. A claim never beats a verified file."""
    v = o.get("verified")
    bits = (v["bit_depth"] * v["sample_rate"]) if v else 0
    c = o.get("claimed") or {}
    claim = (c.get("bit_depth") or 0) * (c.get("sample_rate") or 0)
    return (
        1 if v and is_hi_res(v) else 0,
        bits,
        o.get("match", 0),
        claim,
        (o.get("seeders") or 0) + (1 if o.get("web_seed") else 0),
    )


def redact(s):
    return re.sub(r"(?i)(apikey|passkey|token|key)=[^&\s\"']+", r"\1=***", str(s))


def sane_magnet(m):
    return isinstance(m, str) and m.startswith("magnet:?") and len(m) < 8192


def parse_torznab(xml_bytes, q, indexer):
    root = ET.fromstring(xml_bytes)
    out = []
    for item in root.iter("item"):
        attrs = {}
        for e in item:
            if e.tag.endswith("}attr"):
                attrs[e.get("name")] = e.get("value")
        title = item.findtext("title") or ""
        enc = item.find("enclosure")
        link = item.findtext("link") or (enc.get("url") if enc is not None else "")
        magnet = attrs.get("magneturl") or (link if link.startswith("magnet:") else "")
        size = item.findtext("size") or attrs.get("size") or (enc.get("length") if enc is not None else 0)
        try:
            size = int(size)
        except (TypeError, ValueError):
            size = 0
        seeders = attrs.get("seeders")
        lower = title.lower()
        is_flac = "flac" in lower or "lossless" in lower
        score, why = match_score(q, path=title, is_flac=is_flac)
        if score < 0.5:
            continue
        out.append({
            "source": "torznab",
            "indexer": indexer,
            "name": title,
            "torrent_url": "" if link.startswith("magnet:") else link,
            "magnet": magnet if sane_magnet(magnet) else "",
            "size": size,
            "size_is": "whole torrent, claimed by the indexer",
            "seeders": int(seeders) if seeders and seeders.isdigit() else None,
            "seeders_from": "indexer" if seeders else None,
            "web_seed": False,
            "claimed": claimed_from_name(title),
            "match": score,
            "match_why": why,
            "file": None,
            "license": None,
        })
    return out


def pick_file(files, q, hint=None):
    """Chooses the FLAC inside a torrent. Returns (entry, score) or (None, 0)."""
    best, best_s = None, 0
    for f in files:
        if not f["path"].lower().endswith(".flac"):
            continue
        if hint and os.path.basename(f["path"]) == hint:
            return f, 1.0
        if hint:
            continue
        s, _ = match_score(q, path=f["path"])
        if s > best_s:
            best, best_s = f, s
    return best, best_s


def evict(cache_root, limit_bytes, keep=()):
    """Oldest first, never what is in use."""
    entries = []
    total = 0
    for name in os.listdir(cache_root) if os.path.isdir(cache_root) else []:
        p = os.path.join(cache_root, name)
        size = 0
        for r, _, fs in os.walk(p):
            for f in fs:
                try:
                    size += os.stat(os.path.join(r, f)).st_blocks * 512
                except OSError:
                    pass
        total += size
        if name not in keep:
            entries.append((os.path.getmtime(p), size, p))
    entries.sort()
    removed = []
    while total > limit_bytes and entries:
        _, size, p = entries.pop(0)
        shutil.rmtree(p, ignore_errors=True)
        total -= size
        removed.append(os.path.basename(p))
    return removed, total


# --- network ------------------------------------------------------------------
def http_get(url, limit, timeout=20):
    if not url.lower().startswith(("http://", "https://")):
        raise ValueError("only http and https")
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        data = r.read(limit + 1)
    if len(data) > limit:
        raise ValueError("response too large")
    return data


def ia_query(q, collections):
    def esc(s):
        return re.sub(r'([+\-&|!(){}\[\]^"~*?:\\/])', " ", s or "").strip()

    art, tit = esc(q.get("artist")), esc(q.get("title"))
    who = f'(creator:({art}) OR title:({art}) OR subject:({art}))' if art else "*:*"
    what = f'(title:({tit}) OR identifier:({tit.replace(" ", "")}))' if tit else "*:*"
    coll = " OR ".join(collections)
    return (
        f"mediatype:audio AND collection:({coll}) AND format:(Flac OR \"24bit Flac\") "
        f"AND licenseurl:(*creativecommons.org*) AND {who} AND {what}"
    )


def ia_search(q, settings, get=http_get, max_items=6):
    args = [("q", ia_query(q, settings["ia_collections"])), ("output", "json"), ("rows", str(max_items)),
            ("fl[]", "identifier"), ("fl[]", "title"), ("fl[]", "creator"), ("fl[]", "licenseurl"),
            ("sort[]", "downloads desc")]
    d = json.loads(get("https://archive.org/advancedsearch.php?" + urllib.parse.urlencode(args), MAX_XML))
    out = []
    for doc in d["response"]["docs"]:
        ident = doc["identifier"]
        meta = json.loads(get("https://archive.org/metadata/" + urllib.parse.quote(ident), MAX_XML))
        files = meta.get("files") or []
        base = os.path.dirname(meta.get("dir") or "")
        webseeds = [f"https://{meta[k]}{base}/" for k in ("d1", "d2") if meta.get(k) and base]
        torrents = [f["name"] for f in files if f.get("format") == "Archive BitTorrent"]
        if not torrents:
            continue
        creator = doc.get("creator") or ""
        if isinstance(creator, list):
            creator = ", ".join(creator)
        for f in files:
            fmt = f.get("format") or ""
            if "flac" not in fmt.lower() or "fingerprint" in fmt.lower() or not f["name"].lower().endswith(".flac"):
                continue
            dur = f.get("length")
            try:
                dur = float(dur) if dur and ":" not in str(dur) else None
            except ValueError:
                dur = None
            score, why = match_score(q, artist=f.get("creator") or creator, title=f.get("title") or doc.get("title", ""),
                                     path=f["name"], duration=dur)
            if score < 0.5:
                continue
            claim = {"bit_depth": 24} if fmt.lower().startswith("24bit") else {}
            claim.update(claimed_from_name(f["name"]))
            out.append({
                "source": "internet_archive",
                "indexer": "archive.org/details/" + ident,
                "name": f"{f.get('title') or f['name']} ({creator or ident})",
                "torrent_url": f"https://archive.org/download/{urllib.parse.quote(ident)}/{urllib.parse.quote(torrents[0])}",
                "magnet": "",
                "size": int(f.get("size") or 0),
                "size_is": "this file, per archive.org",
                "seeders": None,
                "seeders_from": None,
                "web_seed": True,
                "webseeds": webseeds,
                "claimed": claim,
                "claimed_duration": dur,
                "match": score,
                "match_why": why,
                "file": f["name"],
                "license": doc.get("licenseurl"),
                "license_is": "stated by the uploader, not checked by ZroovMusic",
            })
    return out


def tl(what):
    path = os.environ.get("ZROOV_TIMELINE")
    if path:
        with open(path, "a") as f:
            f.write(f"{int(time.time() * 1000)} py {what}\n")


# --- the source ---------------------------------------------------------------
def split_query(text):
    """The engine passes one string: JSON fields, or "Artist - Title", or a title."""
    text = (text or "").strip()
    if text.startswith("{"):
        d = json.loads(text)
        return {k: d.get(k) for k in ("artist", "title", "duration")}
    if " - " in text:
        a, t = text.split(" - ", 1)
        return {"artist": a.strip(), "title": t.strip(), "duration": None}
    return {"artist": "", "title": text, "duration": None}


class Source:
    def __init__(self, out=None, home=None, helper=None):
        self.out = out or sys.stdout
        home = home or os.path.expanduser("~")
        cfg = os.environ.get("XDG_CONFIG_HOME") or os.path.join(home, ".config")
        cache = os.environ.get("XDG_CACHE_HOME") or os.path.join(home, ".cache")
        self.cfg_path = os.path.join(cfg, "zroovmusic", "torrent.json")
        self.cache = os.path.join(cache, "zroovmusic", "torrent")
        # Outside the evicted tree. Offers can hold indexer links, so 0600.
        self.offers_path = os.path.join(cache, "zroovmusic", "torrent-offers.json")
        self.helper = helper or os.environ.get("ZROOV_BTSTREAM") or os.path.join(
            os.path.dirname(os.path.realpath(__file__)), "zrn-zroov-btstream")
        self.state = self._load(self.cfg_path, {"consent": 0, "settings": {}})
        self.offers = self._load(self.offers_path, {})
        self.child = None

    @staticmethod
    def _load(path, base):
        try:
            with open(path) as f:
                base.update(json.load(f))
        except (OSError, ValueError):
            pass
        return base

    @staticmethod
    def _write(path, data):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = path + ".tmp"
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            json.dump(data, f, indent=1)
        os.replace(tmp, path)

    def _save_offers(self):
        keep = sorted(self.offers.items(), key=lambda kv: kv[1].get("at", 0))[-200:]
        self.offers = dict(keep)
        self._write(self.offers_path, self.offers)

    @property
    def settings(self):
        return {**DEFAULTS, **self.state["settings"]}

    def emit(self, type_, **kw):
        self.out.write(json.dumps({"type": type_, **kw}) + "\n")
        self.out.flush()

    def consented(self):
        return self.state.get("consent") == CONSENT_VERSION

    # one-shot commands for the UI
    def disclosure(self):
        self.emit("disclosure", version=CONSENT_VERSION, text=DISCLOSURE, accepted=self.consented())

    def consent(self, accept):
        self.state["consent"] = CONSENT_VERSION if accept else 0
        self._write(self.cfg_path, self.state)
        self.emit("consent", accepted=accept)

    def set_settings(self, new):
        new = {k: v for k, v in (new or {}).items() if k in DEFAULTS}
        if new:
            self.state["settings"].update(new)
            self._write(self.cfg_path, self.state)
        shown = dict(self.settings)
        shown["torznab_urls"] = [redact(u) for u in shown["torznab_urls"]]
        self.emit("settings", settings=shown, consent=self.consented())

    def clear_cache(self):
        removed, total = evict(self.cache, 0)
        self.emit("cache", removed=len(removed), bytes=total)

    # engine: search
    def search(self, text, limit=20):
        q = split_query(text)
        found = []
        s = self.settings
        if s["internet_archive"]:
            try:
                found += ia_search(q, s)
            except Exception as e:
                print("internet_archive: " + redact(e), file=sys.stderr)
        for url in s["torznab_urls"]:
            try:
                found += self._torznab(url, q)
            except Exception as e:
                print(redact(url) + ": " + redact(e), file=sys.stderr)
        now = time.time()
        for o in found:
            key = (o["torrent_url"] or o["magnet"] or o["name"]) + "\0" + (o["file"] or "")
            o["id"] = hashlib.sha1(key.encode()).hexdigest()[:16]
            old = self.offers.get(o["id"]) or {}
            o.update(verified=old.get("verified"), verified_note=old.get("verified_note"),
                     state=old.get("state", "offered"), query=q, at=now)
            if old.get("seeders_from") == "tracker scrape":
                o["seeders"], o["seeders_from"] = old["seeders"], old["seeders_from"]
            self.offers[o["id"]] = o
        if found:
            self._save_offers()
        for o in sorted(found, key=rank_key, reverse=True)[: int(limit)]:
            self.emit("result", **self.track(o))
        self.emit("done")

    def _torznab(self, url, q):
        sep = "&" if "?" in url else "?"
        args = {"t": "music", "artist": q.get("artist") or "", "track": q.get("title") or "", "cat": "3040"}
        host = urllib.parse.urlsplit(url).hostname or "indexer"
        try:
            found = parse_torznab(http_get(url + sep + urllib.parse.urlencode(args), MAX_XML), q, host)
        except Exception:
            found = []
        if not found:  # not every indexer has t=music
            args = {"t": "search", "q": f"{q.get('artist') or ''} {q.get('title') or ''} flac".strip()}
            found = parse_torznab(http_get(url + sep + urllib.parse.urlencode(args), MAX_XML), q, host)
        return found

    def track(self, o):
        """An engine Track. The engine returns unknown fields, so the offer travels with it."""
        q = o.get("query") or {}
        return {
            "uri": "torrent:" + o["id"], "title": q.get("title") or o["name"], "artist": q.get("artist") or "",
            "duration": (o.get("verified") or {}).get("duration") or o.get("claimed_duration") or q.get("duration") or 0,
            "offer": self.public(o),
        }

    @staticmethod
    def public(o):
        v = o.get("verified")
        quality = v or o.get("claimed") or {}
        return {
            "offer": o["id"], "source": o["source"], "indexer": o["indexer"], "name": o["name"], "file": o["file"],
            "size": o["size"], "size_is": o["size_is"],
            "seeders": o["seeders"], "seeders_from": o["seeders_from"], "web_seed": o["web_seed"],
            "quality": {
                "bit_depth": quality.get("bit_depth"), "sample_rate": quality.get("sample_rate"),
                "channels": quality.get("channels"), "duration": quality.get("duration"),
                "hi_res": is_hi_res(quality),
                "status": "verified" if v else "claimed" if o.get("claimed") else "unknown",
            },
            "verified": v, "verified_note": o.get("verified_note"), "claimed": o.get("claimed"),
            "match": o["match"], "match_why": o["match_why"], "license": o.get("license"), "license_is": o.get("license_is"),
            "state": o["state"], "autostart": False,
        }

    def _helper_args(self, o, mode):
        s = self.settings
        os.makedirs(self.cache, exist_ok=True)
        key = hashlib.sha1((o["torrent_url"] or o["magnet"]).encode()).hexdigest()
        d = os.path.join(self.cache, key)
        os.makedirs(d, exist_ok=True)
        os.utime(d)
        cap = int(s["cache_mib"]) << 20
        if o.get("size") and o["file"] and o["size"] > cap:
            raise ValueError("file is larger than the cache limit")
        evict(self.cache, max(cap - int(o.get("size") or 0), 0), keep=(key,))
        args = [self.helper, "--dir", d, "--mode", mode, "--upload-kib", str(s["upload_kib"]),
                "--download-kib", str(s["download_kib"]), "--wifi-only", "1" if s["wifi_only"] else "0",
                "--idle-stop-s", str(s["idle_stop_s"]), "--seed-after-s", str(s["seed_after_s"])]
        if o["torrent_url"]:
            path = os.path.join(d, "meta.torrent")
            if not os.path.exists(path):
                with open(path + ".tmp", "wb") as f:
                    f.write(http_get(o["torrent_url"], MAX_TORRENT, timeout=30))
                os.replace(path + ".tmp", path)
            args += ["--torrent", path]
        elif o["magnet"]:
            args += ["--magnet", o["magnet"], "--dht", "1" if s["dht_for_magnets"] else "0"]
        else:
            raise ValueError("offer has no torrent")
        for u in o.get("webseeds") or []:
            args += ["--webseed", u]
        return args

    @staticmethod
    def _spawn(args):
        return subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)

    def _select(self, p, o, meta):
        f, score = pick_file(meta["files"], o.get("query") or {}, os.path.basename(o["file"]) if o["file"] else None)
        if f is None or (not o["file"] and score < 0.5):
            return None
        if o["size"] and o["file"] and f["size"] != o["size"]:
            print("torrent is older than the item; file size differs", file=sys.stderr)
        o["file"] = f["path"]
        o["size"] = f["size"]
        o["size_is"] = "this file, from the torrent's own list"
        p.stdin.write(f"select {f['i']}\n")
        p.stdin.flush()
        return f

    def _apply_header(self, o, raw):
        """Returns why the file must not be offered, or None."""
        info = parse_flac_header(raw)
        if info is None:
            o["state"] = "rejected_not_flac"
            return "file is not FLAC, whatever its name says"
        o["verified"] = info
        o["verified_note"] = VERIFIED_NOTE
        o["state"] = "verified"
        want = (o.get("query") or {}).get("duration")
        if want and info["duration"] and abs(float(want) - info["duration"]) > 20:
            o["state"] = "rejected_duration"
            o["match"] = min(o["match"], 0.4)
            return "length differs from the requested song"
        return None

    # verify: first pieces only, then out of the swarm
    def verify(self, ids):
        if not self.consented():
            self.emit("error", reason="consent_required", text=DISCLOSURE)
            return
        done = []
        for oid in ids:
            o = self.offers.get(oid)
            if not o:
                self.emit("verify", offer=oid, ok=False, what="unknown offer")
                continue
            self._verify_one(o)
            done.append(o)
        self._save_offers()
        for o in sorted(done, key=rank_key, reverse=True):
            self.emit("offer", **self.track(o))

    def _verify_one(self, o):
        t0 = time.monotonic()
        try:
            p = self._spawn(self._helper_args(o, "probe"))
        except Exception as e:
            self.emit("verify", offer=o["id"], ok=False, what=redact(e))
            return
        cost = {}
        timer = threading.Timer(HEAD_TIMEOUT_S, p.terminate)
        timer.start()
        failed = "no pieces arrived in time"
        for line in p.stdout:
            ev = json.loads(line)
            k = ev["ev"]
            if k == "meta" and not self._select(p, o, ev):
                failed = "no matching FLAC inside the torrent"
                p.stdin.write("stop\n")
                p.stdin.flush()
            elif k == "head":
                cost = {"down": ev["down"], "up": ev["up"], "ms": ev["ms"]}
                failed = self._apply_header(o, bytes.fromhex(ev["hex"]))
            elif k == "swarm":
                o["seeders"], o["seeders_from"] = ev["seeders"], "tracker scrape"
            elif k == "network_blocked":
                failed = "not on Wi-Fi"
            elif k == "error":
                failed = ev["what"]
        timer.cancel()
        p.stdin.close()
        p.wait()
        self.emit("verify", offer=o["id"], ok=failed is None, what=failed, seconds=round(time.monotonic() - t0, 2),
                  verified=o.get("verified"), note=o.get("verified_note"), cost=cost)

    # engine: resolve. The user picked this offer, and only this starts a stream.
    def resolve(self, ref):
        if os.environ.get("ZROOV_PREFETCH"):
            # The engine resolves the next track early. Queued is not chosen to start:
            # no swarm until this track is really the current one.
            self.emit("error", reason="torrent streams do not prefetch")
            return 0
        if not self.consented():
            self.emit("error", reason="consent_required: " + DISCLOSURE)
            return 0
        tl("resolve")
        o = self.offers.get(ref)
        if not o:
            self.emit("error", reason="unknown offer, search again")
            return 0
        try:
            args = self._helper_args(o, "play")
            tl("torrent file ready")
        except Exception as e:
            self.emit("error", reason=redact(e))
            return 0
        known = (o.get("verified") or {}).get("duration") or o.get("claimed_duration")
        rate = o["size"] / known if known and o["size"] else 200_000
        args += ["--buffer-bytes", str(int(rate * float(self.settings["buffer_seconds"]))),
                 "--bytes-per-s", str(int(rate)),
                 "--ahead-low-s", str(self.settings["ahead_low_s"]), "--ahead-high-s", str(self.settings["ahead_high_s"])]
        p = self.child = self._spawn(args)

        def on_term(*_):
            # The engine left this track: leave the swarm unless the user asked to share on.
            try:
                p.stdin.write("stop\n")
                p.stdin.flush()
            except (OSError, ValueError):
                pass

        signal.signal(signal.SIGTERM, on_term)
        signal.signal(signal.SIGINT, on_term)
        answered = False
        for line in p.stdout:
            ev = json.loads(line)
            k = ev["ev"]
            if k == "meta":
                if not self._select(p, o, ev):
                    self.emit("error", reason="no matching FLAC inside the torrent")
                    answered = True
                    on_term()
            elif k == "ready" and not answered:
                tl("helper ready")
                why = self._read_own_header(o, ev["url"])
                tl("own header read")
                self._save_offers()
                answered = True
                if why:
                    self.emit("error", reason=why)
                    on_term()
                    continue
                v = o["verified"]
                if v["duration"] and o["size"]:  # the real rate, also when no length was known
                    p.stdin.write(f"rate {int(o['size'] / v['duration'])}\n")
                    p.stdin.flush()
                self.emit("stream", url=ev["url"], via="torrent", hold=True, title="", artist="",
                          duration=v["duration"] or 0, bits=v["bit_depth"], offer=self.public(o), ready_ms=ev["ms"])
            elif k == "stats":
                text = f"{ev['peers']} peers, down {ev['down_bps'] // 1024} KiB/s, up {ev['up_bps'] // 1024} KiB/s"
                if not ev["paused"]:
                    self.emit("status", text=text, stats=ev)
            elif k == "swarm":
                o["seeders"], o["seeders_from"] = ev["seeders"], "tracker scrape"
            elif k == "network_blocked" and not answered:
                answered = True
                self.emit("error", reason="not on Wi-Fi; torrents are set to Wi-Fi only")
            elif k == "network_blocked":
                self.emit("status", text="paused: not on Wi-Fi")
            elif k == "error" and not answered:
                answered = True
                self.emit("error", reason=ev["what"])
            elif k == "end":
                self.emit("status", text=f"left the swarm, {ev['down']} bytes down, {ev['up']} up", end=ev)
        p.wait()
        if not answered:
            self.emit("error", reason="source ended without a stream")
        return 0

    @staticmethod
    def _read_own_header(o, url):
        req = urllib.request.Request(url, headers={"Range": "bytes=0-65535"})
        raw = b""
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                raw = r.read(65536)
        except OSError as e:
            return "could not read the file head: " + str(e)
        return Source._apply_header(Source, o, raw)


def main(argv):
    if len(argv) < 2:
        print("usage: zroov-source-torrent search QUERY LIMIT | resolve REF | fetch REF DIR | verify REF... | "
              "disclosure | consent yes|no | settings [JSON] | clear-cache", file=sys.stderr)
        return 2
    s, c = Source(), argv[1]
    if c == "search":
        s.search(argv[2] if len(argv) > 2 else "", argv[3] if len(argv) > 3 else 20)
    elif c == "resolve":
        return s.resolve(argv[2])
    elif c == "fetch":
        s.emit("error", reason="torrent offers stream only; saving offline is not offered")
    elif c == "verify":
        s.verify(argv[2:])
    elif c == "disclosure":
        s.disclosure()
    elif c == "consent":
        s.consent(argv[2] == "yes")
    elif c == "settings":
        s.set_settings(json.loads(argv[2]) if len(argv) > 2 else None)
    elif c == "clear-cache":
        s.clear_cache()
    else:
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
