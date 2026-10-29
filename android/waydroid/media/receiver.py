#!/usr/bin/env python3
"""Android's media sessions, from the Waydroid bridge app, as tile signals.

Run as the desktop session user. The bridge app (MediaListener.java) POSTs one
JSON object per change:

    {"package": "com.spotify.music", "state": "playing|paused|stopped",
     "title": "...", "artist": "...", "album": "...", "art": "<base64 jpeg>"}

and this turns it into a line on the shell's control pipe:

    signal com.spotify.music|title=Playing|line=Title|line=Artist|badge=play|image=PATH

for the tile of that app (the shell finds it by package), or `signal PACKAGE`
alone when it stopped. See ztmetro/TILE-SIGNALS.md. Blocks in accept()
between events: no timers, nothing to wake it.
"""
import argparse
import base64
import binascii
import hmac
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
from pathlib import Path
import re

MAX_BODY = 262144
PACKAGE = re.compile(r"^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z0-9_]+)+$")
CONTAINER_NET = "192.168.240."   # Waydroid's bridge; nothing else may post


def runtime():
    return os.environ.get("XDG_RUNTIME_DIR") or "/run/user/%d" % os.getuid()


def field(value, limit):
    if not isinstance(value, str):
        return ""
    return value.replace("|", "/").replace("\n", " ").replace("\r", " ").replace("\x00", "")[:limit].strip()


def tell_shell(line):
    """One line to the shell; no shell, nobody to show it to."""
    try:
        fd = os.open(os.path.join(runtime(), "ztmetro.ctl"), os.O_WRONLY | os.O_NONBLOCK | os.O_CLOEXEC)
    except OSError:
        return
    try:
        os.write(fd, (line + "\n").encode())
    except OSError:
        pass
    finally:
        os.close(fd)


def deliver(data):
    package = data.get("package")
    if not isinstance(package, str) or not PACKAGE.match(package) or len(package) > 160:
        raise ValueError("package")
    state = data.get("state")
    art_path = Path(runtime(), "ztos-art", package + ".jpg")
    if state not in ("playing", "paused"):
        tell_shell("signal " + package)
        art_path.unlink(missing_ok=True)
        return
    art = data.get("art")
    if isinstance(art, str) and art:
        try:
            jpeg = base64.b64decode(art, validate=True)
        except (binascii.Error, ValueError):
            jpeg = b""
        if 0 < len(jpeg) <= 200000:
            art_path.parent.mkdir(mode=0o700, exist_ok=True)
            tmp = art_path.with_suffix(".part")
            tmp.write_bytes(jpeg)
            os.replace(tmp, art_path)
    title = field(data.get("title"), 120) or "Unknown"
    parts = ["signal " + package, "title=" + ("Playing" if state == "playing" else "Paused"), "line=" + title]
    for key in ("artist", "album"):
        value = field(data.get(key), 100)
        if value:
            parts.append("line=" + value)
    parts.append("badge=" + ("play" if state == "playing" else "pause"))
    if art_path.exists():
        parts.append("image=" + str(art_path))
    tell_shell("|".join(parts))


class Handler(BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(5)

    def log_message(self, *_):
        pass  # what is playing is not for logs

    def do_POST(self):
        if self.path != "/media" or not self.client_address[0].startswith(CONTAINER_NET):
            self.send_error(404)
            return
        supplied = self.headers.get("Authorization", "")
        if not hmac.compare_digest(supplied, "Bearer " + self.server.token):
            self.send_error(403)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= MAX_BODY or self.headers.get("Transfer-Encoding"):
                raise ValueError("length")
            raw = self.rfile.read(length)
            if len(raw) != length:
                raise ValueError("short")
            data = json.loads(raw)
            if not isinstance(data, dict):
                raise ValueError("object")
            deliver(data)
        except (ValueError, UnicodeError):
            self.send_error(400)
            return
        self.send_response(204)
        self.end_headers()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=17893)
    parser.add_argument("--token-file", type=Path, required=True)
    args = parser.parse_args()
    if args.token_file.stat().st_mode & 0o077:
        parser.error("token file must be private (chmod 600)")
    token = args.token_file.read_text().strip()
    if len(token) < 32 or not token.isascii():
        parser.error("token needs at least 32 ASCII characters")
    server = HTTPServer((args.bind, args.port), Handler)
    server.token = token
    while True:
        server.handle_request()


if __name__ == "__main__":
    main()
