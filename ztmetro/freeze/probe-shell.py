#!/usr/bin/env python3
"""Probe shell event-loop responsiveness at 20 Hz during the browsing check."""
import json
import os
import pathlib
import time
import sys

sys.path.insert(0, '/usr/lib/ztos/python')
import ztos_user

log_path = ztos_user.home('.local/state/ztmetro.log')
ctl_path = str(ztos_user.runtime('ztmetro.ctl'))
output = ztos_user.home('freeze-evidence/shell-probes-final.jsonl')
end = time.monotonic() + 420
with log_path.open() as log, output.open('w') as stream:
    while time.monotonic() < end:
        log.seek(0, 2)
        began = time.monotonic()
        fd = os.open(ctl_path, os.O_WRONLY | os.O_NONBLOCK)
        try:
            os.write(fd, b'state\n')
        finally:
            os.close(fd)
        replied = False
        received = ""
        while time.monotonic() - began < 1:
            received += log.read()
            if 'state: front=' in received:
                replied = True
                break
            time.sleep(0.001)
        latency = (time.monotonic() - began) * 1000
        stream.write(json.dumps({'monotonic': began, 'latency_ms': latency, 'replied': replied}) + '\n')
        stream.flush()
        time.sleep(max(0, 0.05 - (time.monotonic() - began)))
