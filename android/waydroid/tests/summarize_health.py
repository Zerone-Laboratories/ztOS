#!/usr/bin/env python3
"""Summarize raw health_sample JSONL, retaining measurement caveats."""
from collections import defaultdict
import json
from pathlib import Path
import sys


def values(text):
    return dict(line.split()[:2] for line in (text or '').splitlines() if len(line.split()) >= 2)


def interrupts(text):
    lines = (text or '').splitlines()
    count = len(lines[0].split()) if lines else 0
    result = {}
    for line in lines[1:]:
        fields = line.split()
        try:
            result[fields[0] + ' ' + ' '.join(fields[count + 1:])] = sum(map(int, fields[1:count + 1]))
        except (ValueError, IndexError):
            pass
    return result


def summarize(path):
    rows = [json.loads(line) for line in path.read_text().splitlines()]
    first, last = rows[0], rows[-1]
    duration = last['monotonic'] - first['monotonic']
    if duration <= 0:
        return {'file': str(path), 'samples': len(rows), 'error': 'No elapsed interval'}
    current_integral = 0
    cpu_seconds = 0
    process_seconds = defaultdict(float)
    issues = set()
    irq_delta = defaultdict(int)
    for a, b in zip(rows, rows[1:]):
        elapsed = b['monotonic'] - a['monotonic']
        current_integral += (int(a['battery']['current_now']) + int(b['battery']['current_now'])) / 2 * elapsed
        delta = int(values(b['cpu_stat'])['usage_usec']) - int(values(a['cpu_stat'])['usage_usec'])
        if delta < 0 or a.get('container_inode') != b.get('container_inode'):
            issues.add('Container CPU counters reset; CPU total is incomplete')
        else:
            cpu_seconds += delta / 1e6
        if a.get('boot_id') != b.get('boot_id'):
            issues.add('Host rebooted')
        hz = b.get('clock_ticks', 100)
        for pid, process in b['processes'].items():
            previous = a['processes'].get(pid)
            if previous and previous['start'] == process['start']:
                ticks = process['ticks'] - previous['ticks']
            elif process['start'] / hz >= a['monotonic']:
                ticks = process['ticks']
            else:
                continue
            process_seconds[process.get('service') or process['name']] += max(0, ticks) / hz
        ia, ib = interrupts(a['interrupts']), interrupts(b['interrupts'])
        for key, value in ib.items():
            if key in ia:
                irq_delta[key] += max(0, value - ia[key])
    statuses = sorted({row['battery']['status'] for row in rows})
    if any('POWER_SUPPLY_ONLINE=1' in (row['charger'] or '') for row in rows):
        issues.add('USB-connected net battery current; not battery-only system drain')
    screens = sorted({values(row['perfd'].replace('=', ' ')).get('screen') for row in rows})
    if screens != ['off']:
        issues.add('Screen was not off throughout')
    charge_delta = int(last['battery']['charge_counter']) - int(first['battery']['charge_counter'])
    return {
        'file': str(path), 'start': first['utc'], 'end': last['utc'],
        'samples': len(rows), 'seconds': round(duration, 3), 'screens': screens,
        'freeze_states': sorted({row['freeze'] for row in rows}), 'battery_status': statuses,
        'mean_net_current_mA': round(current_integral / duration / 1000, 3),
        'charge_counter_delta_mAh': round(charge_delta / 1000, 3),
        'charge_counter_rate_mA': round(charge_delta * 3.6 / duration, 3),
        'android_cpu_seconds': round(cpu_seconds, 3),
        'android_cpu_percent_one_core': round(cpu_seconds / duration * 100, 3),
        'irq_per_second': round(sum(irq_delta.values()) / duration, 3),
        'top_irq_per_second': {k: round(v / duration, 3) for k, v in sorted(irq_delta.items(), key=lambda item: item[1], reverse=True)[:10]},
        'process_cpu_percent_one_core': {k: round(v / duration * 100, 3) for k, v in sorted(process_seconds.items(), key=lambda item: item[1], reverse=True)[:20]},
        'tracked_cpu_percent_one_core': {name: round(process_seconds[name] / duration * 100, 3)
                                         if name in process_seconds else None
                                         for name in ('zrnperfd', 'zrn-sleepd', 'ztos-waydroid-network')},
        'issues': sorted(issues),
    }


if __name__ == '__main__':
    reports = [summarize(Path(argument)) for argument in sys.argv[1:]]
    print(json.dumps(reports[0] if len(reports) == 1 else reports, indent=2))
