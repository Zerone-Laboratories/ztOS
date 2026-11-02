#!/usr/bin/env python3
"""A stand-in for zroovd with invented tracks: the UI's half of ENGINE.md, no audio."""
import json, os, selectors, socket, time

D = os.path.join(os.environ["XDG_RUNTIME_DIR"], "zroovmusic")
os.makedirs(D, exist_ok=True)
P = os.path.join(D, "engine.sock")
if os.path.exists(P):
    os.unlink(P)
srv = socket.socket(socket.AF_UNIX)
srv.bind(P)
srv.listen()
ART = os.environ.get("ZV_ART", "")
NAMES = "Amber Road,Bright Water,Copper Sky,Dust Line,Eastern Glass,Fable,Grey Harbour,Hollow Tide,Iron Lace,June".split(",")
TR = [{"uri": "file:/invented/%d" % i, "title": n, "artist": "Invented Band", "album": "Test album", "art": ART, "duration": 163 + i}
      for i, n in enumerate(NAMES * 6)]
OFFER = {"uri": "torrent:abc123", "title": "Amber Road", "artist": "Invented Band", "duration": 163,
         "offer": {"offer": "abc123", "indexer": "invented.example", "name": "Invented Band - Amber Road (FLAC)",
                   "seeders": 12, "license": "CC BY", "match_why": "title and artist match", "state": "offered",
                   "quality": {"bit_depth": 24, "sample_rate": 96000, "status": "claimed", "hi_res": True}}}
s = {"state": "stopped", "index": 0, "pos": 0.0, "pos_at": time.monotonic(), "shuffle": False, "repeat": "off"}
queue = TR[:12]
clients = []
sel = selectors.DefaultSelector()
sel.register(srv, selectors.EVENT_READ)


def now():
    return s["pos"] + (time.monotonic() - s["pos_at"] if s["state"] == "playing" else 0)


def send(obj, to=None):
    b = (json.dumps(obj) + "\n").encode()
    for c in [to] if to else list(clients):
        try:
            c.sendall(b)
        except OSError:
            pass


def state(to=None):
    t = queue[s["index"]] if queue and s["state"] != "stopped" else None
    send({"ev": "state", **s, "track": t, "duration": t["duration"] if t else 0, "volume": 100, "via": "file",
          "cached_to": 0, "format": {"codec": "flac", "rate": 96000, "bits": 24} if t else None}, to)


def q(to=None):
    send({"ev": "queue", "tracks": queue, "order": list(range(len(queue))), "index": s["index"]}, to)


def jump(i):
    s.update(index=i % max(len(queue), 1), pos=0.0, pos_at=time.monotonic(), state="playing")


def handle(c, m):
    global queue
    k = m.get("cmd")
    if k == "get":
        state(c); q(c); return
    if k == "saved":
        send({"ev": "saved", "tracks": TR}, c); return
    if k == "play":
        if "tracks" in m:
            queue = m["tracks"]
        jump(m.get("index", 0)); q()
    elif k == "toggle":
        s.update(pos=now(), pos_at=time.monotonic(), state="paused" if s["state"] == "playing" else "playing")
    elif k == "next":
        jump(s["index"] + 1); q()
    elif k == "previous":
        jump(s["index"] - 1); q()
    elif k == "seek":
        s.update(pos=float(m.get("pos", 0)), pos_at=time.monotonic())
    elif k == "shuffle":
        s["shuffle"] = bool(m.get("on"))
    elif k == "repeat":
        s["repeat"] = m.get("mode", "off")
    elif k == "search":
        res = [OFFER] if m.get("source") == "torrent" else TR[:8]
        send({"ev": "search", "results": res, "done": True}, c); return
    elif k == "quit":
        raise SystemExit
    state()


buf = {}
while True:
    for key, _ in sel.select():
        o = key.fileobj
        if o is srv:
            c, _a = srv.accept()
            clients.append(c); buf[c] = b""
            sel.register(c, selectors.EVENT_READ)
            continue
        d = o.recv(65536)
        if not d:
            sel.unregister(o); clients.remove(o); o.close(); continue
        buf[o] += d
        while b"\n" in buf[o]:
            line, buf[o] = buf[o].split(b"\n", 1)
            handle(o, json.loads(line))
