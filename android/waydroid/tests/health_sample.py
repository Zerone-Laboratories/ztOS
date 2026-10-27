#!/usr/bin/env python3
"""Read-only, finite phone audit. Run as root through the phone controller.

Samples once per minute; never attaches to or wakes frozen Android. Process
start ticks distinguish PID reuse. Raw IRQ counters permit endpoint deltas.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time


def read(path):
    try:
        return Path(path).read_text().strip()
    except OSError:
        return None


def snapshot():
    processes = {}
    for path in Path('/proc').glob('[0-9]*/stat'):
        try:
            raw = path.read_text()
            fields = raw[raw.rfind(')') + 2:].split()
            cgroup = read(path.parent / 'cgroup') or ''
            service = next((name for name in ('zrn-sleepd', 'ztos-waydroid-network')
                            if f'/{name}.service' in cgroup), None)
            processes[path.parent.name] = {
                'name': raw[raw.find('(') + 1:raw.rfind(')')],
                'service': service,
                'ticks': int(fields[11]) + int(fields[12]),
                'start': int(fields[19]),
            }
        except (OSError, ValueError, IndexError):
            pass
    battery = Path('/sys/class/power_supply/max170xx_battery')
    return {
        'utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        'monotonic': time.monotonic(),
        'boot_id': read('/proc/sys/kernel/random/boot_id'),
        'clock_ticks': os.sysconf('SC_CLK_TCK'),
        'container_inode': Path('/sys/fs/cgroup/lxc.payload.waydroid').stat().st_ino,
        'battery': {key: read(battery / key) for key in
                    ('current_now', 'current_avg', 'charge_counter', 'charge_now',
                     'voltage_now', 'capacity', 'status', 'temp')},
        'charger': read('/sys/class/power_supply/max77705-charger/uevent'),
        'interrupts': read('/proc/interrupts'),
        'stat': read('/proc/stat'),
        'processes': processes,
        'cpu_stat': read('/sys/fs/cgroup/lxc.payload.waydroid/cpu.stat'),
        'freeze': read('/sys/fs/cgroup/lxc.payload.waydroid/cgroup.freeze'),
        'perfd': read('/run/zrnperfd.status'),
        'sleepd': read('/run/zrn-sleepd/status'),
        'online': read('/sys/devices/system/cpu/online'),
        'brightness': {str(p): read(p) for p in Path('/sys/class/backlight').glob('*/brightness')},
        'cpuidle': {str(p): read(p) for p in Path('/sys/devices/system/cpu').glob('cpu*/cpuidle/state*/*')
                    if p.name in ('usage', 'time', 'name', 'disable')},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--seconds', type=int, default=1200)
    args = parser.parse_args()
    start = time.monotonic()
    with args.output.open('w') as stream:
        for elapsed in range(0, args.seconds + 1, 60):
            time.sleep(max(0, start + elapsed - time.monotonic()))
            stream.write(json.dumps(snapshot()) + '\n')
            stream.flush()


if __name__ == '__main__':
    main()
