#!/usr/bin/env python3
"""One finite frozen audio-server restart; never thaw Android for this check."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import sys

sys.path.insert(0, '/usr/lib/ztos/python')
import ztos_user

os.environ.update(XDG_RUNTIME_DIR=ztos_user.RUNTIME_DIR,
                  DBUS_SESSION_BUS_ADDRESS=ztos_user.BUS)
PAYLOAD = Path('/sys/fs/cgroup/lxc.payload.waydroid')
GENERATION = Path('/run/ztos-waydroid-audio.generation')


def snapshot(label):
    stats = dict(line.split() for line in (PAYLOAD / 'cpu.stat').read_text().splitlines())
    value = {'label': label, 'freeze': (PAYLOAD / 'cgroup.freeze').read_text().strip(),
             'usage_usec': stats['usage_usec'], 'generation': GENERATION.read_text()}
    print(json.dumps(value), flush=True)
    return value


def main():
    before = snapshot('before')
    assert before['freeze'] == '1', 'Prepare Home/frozen state before running this test'
    result = subprocess.run(['systemctl', '--user', 'restart', 'pipewire.service',
                             'pipewire-pulse.service', 'wireplumber.service'],
                            capture_output=True, text=True, timeout=45)
    print(json.dumps({'restart_exit': result.returncode, 'stdout': result.stdout,
                      'stderr': result.stderr}), flush=True)
    result.check_returncode()
    started = time.monotonic()
    for delay in (3, 30):
        time.sleep(max(0, started + delay - time.monotonic()))
        after = snapshot(str(delay))
        for key in ('freeze', 'usage_usec', 'generation'):
            assert before[key] == after[key], (key, before, after)
    unit = subprocess.check_output(['systemctl', '--user', 'show',
          'ztos-waydroid-audio-reconnect.service', '-p', 'ActiveState', '-p', 'MainPID',
          '-p', 'CPUUsageNSec'], text=True)
    print(unit, flush=True)
    fields = dict(line.split('=', 1) for line in unit.splitlines())
    assert fields['MainPID'] == '0' and fields['ActiveState'] == 'inactive', fields
    for name in ('/usr/local/libexec/ztos-waydroid-audio',
                 '/etc/pipewire/pipewire.conf.d/90-ztos-waydroid-latency.conf'):
        path = Path(name)
        print(hashlib.sha256(path.read_bytes()).hexdigest(), name, flush=True)
    print('PASS: audio restart preserved payload freeze, CPU usage and HAL generation', flush=True)


if __name__ == '__main__':
    main()
