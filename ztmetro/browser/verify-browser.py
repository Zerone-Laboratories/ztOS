#!/usr/bin/env python3
"""Phone-only HTTP fixtures for browser download and persistent-cache checks.

Run on the phone: python browser/verify-browser.py
Visit http://127.0.0.1:8768/ in Superscan. Server logs every network request.
Fixtures include a slow PDF attachment, unsupported APK MIME, popup download,
cacheable page/resource, interrupted response, and a UTF-8 history title.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import json
import time

ROOT = Path(__file__).resolve().parent
EVIDENCE = ROOT / 'evidence'
EVIDENCE.mkdir(exist_ok=True)


def pdf():
    chunks = [b'%PDF-1.4\n']
    objects = [b'<< /Type /Catalog /Pages 2 0 R >>',
               b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
               b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 200] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>',
               b'<< /Length 63 >>\nstream\nBT /F1 18 Tf 30 100 Td (Superscan PDF download verified) Tj ET\nendstream',
               b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>']
    # Keep stream length exact so independent readers can parse the document.
    stream = b'BT /F1 18 Tf 30 100 Td (Superscan PDF download verified) Tj ET\n'
    objects[3] = b'<< /Length %d >>\nstream\n' % len(stream) + stream + b'endstream'
    offsets = [0]
    for i, obj in enumerate(objects, 1):
        offsets.append(sum(map(len, chunks)))
        chunks.append(b'%d 0 obj\n' % i + obj + b'\nendobj\n')
    xref = sum(map(len, chunks))
    chunks.append(b'xref\n0 6\n0000000000 65535 f \n')
    chunks.extend(b'%010d 00000 n \n' % offset for offset in offsets[1:])
    chunks.append(b'trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n' % xref)
    return b''.join(chunks) + b'\n%' + b' ' * (2 * 1024 * 1024)


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, fmt, *args):
        record = {'time': time.time(), 'request': fmt % args}
        with (EVIDENCE / 'http-requests.jsonl').open('a') as log:
            log.write(json.dumps(record) + '\n')

    def do_GET(self):
        path = self.path.split('?', 1)[0]
        if path in ('/pdf', '/broken'):
            data, mime, name = pdf(), 'application/pdf', 'Superscan verification.pdf'
        elif path == '/apk':
            apk = EVIDENCE / 'uptodown.apk'
            if not apk.exists():
                self.send_error(404, 'Place a real Uptodown APK in evidence/uptodown.apk first')
                return
            data, mime, name = apk.read_bytes(), 'application/vnd.android.package-archive', 'Uptodown verification.apk'
        elif path == '/cached.js':
            data, mime, name = b'document.body.dataset.cached="yes";', 'text/javascript', None
        else:
            data = '''<!doctype html><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Superscan café — history &amp; cache</title><style>body{font:20px sans-serif}a{display:block;padding:18px}</style>
<h3>Browser verification</h3><a href="/pdf">Download slow PDF</a>
<a href="/pdf" target="_blank">Popup PDF download</a><a href="/apk">APK MIME download</a>
<a href="/broken">Interrupted download</a><a href="/second">Second history page</a>
<a href="/">Visit cached page again</a><script src="/cached.js"></script>'''.encode()
            mime, name = 'text/html; charset=utf-8', None
        self.send_response(200)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store' if name else 'public, max-age=3600')
        if name and path != '/apk':
            self.send_header('Content-Disposition', 'attachment; filename="%s"' % name)
        self.end_headers()
        try:
            chunk_size = 262144 if path == "/apk" else 16384
            for start in range(0, len(data), chunk_size):
                if path == '/broken' and start > 65536:
                    self.close_connection = True
                    return
                self.wfile.write(data[start:start + chunk_size])
                self.wfile.flush()
                if name:
                    time.sleep(0.08)
        except (BrokenPipeError, ConnectionResetError):
            pass


ThreadingHTTPServer(('127.0.0.1', 8768), Handler).serve_forever()
