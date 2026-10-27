#!/usr/bin/env python3
"""Finite 20-minute screen-off A/B trial; restore Android even on SIGTERM.

Keep the user's messaging exemption unchanged. The frozen phase deliberately
pauses notification delivery for its measurement window. USB remains connected.
"""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

sys.path.insert(0, '/usr/lib/ztos/python')
import ztos_user

from health_sample import snapshot

OUTPUT = Path(sys.argv[1])
FREEZE = Path('/sys/fs/cgroup/lxc.payload.waydroid/cgroup.freeze')


def command(*args):
    subprocess.run(args, check=True, timeout=20)


def status(phase):
    (OUTPUT / 'phase').write_text(f'{time.time()} {phase}\n')


def terminate(signum, frame):
    raise SystemExit(f'Interrupted by signal {signum}')


def measure(phase, expected):
    status(phase)
    start = time.monotonic()
    identity = None
    with (OUTPUT / f'{phase}.jsonl').open('w') as stream:
        for elapsed in range(0, 1201, 60):
            time.sleep(max(0, start + elapsed - time.monotonic()))
            sample = snapshot()
            stream.write(json.dumps(sample) + '\n')
            stream.flush()
            current_identity = (sample['boot_id'], sample['container_inode'])
            if identity is not None and current_identity != identity:
                raise RuntimeError(f'{phase}: host or container restarted; trial invalid')
            identity = current_identity
            if sample['freeze'] != expected:
                raise RuntimeError(f'{phase}: container freeze changed unexpectedly')
            if 'screen=off\n' not in (sample['perfd'] or ''):
                raise RuntimeError(f'{phase}: screen turned on; trial invalid')


signal.signal(signal.SIGTERM, terminate)
signal.signal(signal.SIGINT, terminate)
OUTPUT.mkdir(exist_ok=True)
try:
    fd = os.open(ztos_user.runtime('ztmetro.ctl'), os.O_WRONLY | os.O_NONBLOCK)
    try:
        os.write(fd, b'screen off\n')
    finally:
        os.close(fd)
    command('waydroid', 'container', 'unfreeze')
    status('settling-awake')
    time.sleep(30)
    measure('awake', '0')
    command('waydroid', 'container', 'freeze')
    status('settling-frozen')
    time.sleep(5)
    measure('frozen', '1')
except BaseException:
    status('interrupted')
    raise
else:
    status('complete')
finally:
    command('waydroid', 'container', 'unfreeze')
