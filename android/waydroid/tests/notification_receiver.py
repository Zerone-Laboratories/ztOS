#!/usr/bin/env python3
"""Bounded wire-format and shell-boundary checks for the optional receiver."""
import http.client
import importlib.util
import json
from pathlib import Path
import threading
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('receiver', Path(__file__).resolve().parents[1] / 'notification/receiver.py')
receiver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(receiver)


class ReceiverTests(unittest.TestCase):
    def request(self, body, token='test-token', length=None):
        received = []
        server = receiver.HTTPServer(('127.0.0.1', 0), receiver.Handler)
        server.token = 'test-token'
        server.deliver = received.append
        worker = threading.Thread(target=server.handle_request)
        worker.start()
        connection = http.client.HTTPConnection(*server.server_address, timeout=2)
        connection.request('POST', '/notify', body, {
            'Authorization': 'Bearer ' + token,
            'Content-Length': str(len(body) if length is None else length)})
        response = connection.getresponse()
        response.read()
        connection.close()
        worker.join(2)
        server.server_close()
        self.assertFalse(worker.is_alive())
        return response.status, received

    def test_forwards_unicode_and_keeps_shell_metacharacters_literal(self):
        data = {'app': 'com.whatsapp', 'title': '--help', 'text': '✓ $(id); `id`'}
        status, got = self.request(json.dumps(data).encode())
        self.assertEqual(status, 204)
        self.assertEqual(got, [[data['app'], data['title'], data['text']]])
        with patch.object(receiver.subprocess, 'run') as run:
            receiver.forward(got[0])
        self.assertEqual(run.call_args.args[0], ['notify-send', '--app-name=com.whatsapp', '--', '--help', data['text']])
        self.assertNotIn('shell', run.call_args.kwargs)

    def test_rejects_bad_token_without_delivery(self):
        self.assertEqual(self.request(b'{}', token='wrong'), (403, []))

    def test_rejects_oversized_request_before_reading_body(self):
        self.assertEqual(self.request(b'', length=8193), (400, []))

    def test_rejects_invalid_json_and_fields(self):
        for value in (b'not json', b'[]', b'{"app":null}', b'{"app":"ok","text":"\\u0000"}',
                      json.dumps({'app': 'ok', 'title': 'x' * 257}).encode()):
            self.assertEqual(self.request(value), (400, []))


if __name__ == '__main__':
    unittest.main()
