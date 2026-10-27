#!/usr/bin/env python3
"""Event-driven Waydroid notification fallback. Run as the desktop session user."""
import argparse
import hmac
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
from pathlib import Path
import subprocess

MAX_BODY = 8192


def notification(data):
    if not isinstance(data, dict):
        raise ValueError("expected object")
    fields = []
    for key, limit in (("app", 160), ("title", 256), ("text", 2048)):
        value = data.get(key, "")
        if not isinstance(value, str) or len(value) > limit or "\0" in value:
            raise ValueError("invalid " + key)
        fields.append(value)
    if not fields[0]:
        raise ValueError("empty app")
    return fields


def forward(fields):
    # argv and -- keep Android text out of both shell and option parsing.
    subprocess.run(["notify-send", "--app-name=" + fields[0], "--",
                    fields[1], fields[2]], timeout=3, check=True,
                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL)


class Handler(BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(5)

    def log_message(self, *_):
        pass  # Notification bodies and credentials must never enter logs.

    def do_POST(self):
        if self.path != "/notify":
            self.send_error(404)
            return
        supplied = self.headers.get("Authorization", "")
        if not hmac.compare_digest(supplied, "Bearer " + self.server.token):
            self.send_error(403)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= MAX_BODY or self.headers.get("Transfer-Encoding"):
                raise ValueError("invalid length")
            raw = self.rfile.read(length)
            if len(raw) != length:
                raise ValueError("short body")
            fields = notification(json.loads(raw))
        except (ValueError, UnicodeError):
            self.send_error(400)
            return
        try:
            self.server.deliver(fields)
        except (OSError, subprocess.SubprocessError):
            self.send_error(503)
            return
        self.send_response(204)
        self.end_headers()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="192.168.240.1")
    parser.add_argument("--port", type=int, default=17892)
    parser.add_argument("--token-file", type=Path, required=True)
    args = parser.parse_args()
    if args.token_file.stat().st_mode & 0o077:
        parser.error("token file must be private (chmod 600)")
    token = args.token_file.read_text().strip()
    if len(token) < 32 or not token.isascii():
        parser.error("token needs at least 32 ASCII characters")
    server = HTTPServer((args.bind, args.port), Handler)
    server.token = token
    server.deliver = forward
    # handle_request blocks in select indefinitely while idle; no periodic work.
    while True:
        server.handle_request()


if __name__ == "__main__":
    main()
