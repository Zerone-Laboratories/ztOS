#!/usr/bin/env python3
"""Validate recorded trials and summarize measured compositor timestamps."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import re
import statistics


def records(path):
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def compositor_time(line):
    match = re.match(r'(\d+):(\d+):(\d+\.\d+)', line)
    assert match, line
    return int(match[1]) * 3600 + int(match[2]) * 60 + float(match[3])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('results', type=Path)
    args = parser.parse_args()
    exits = [r for r in records(args.results / 'reliability-final-exits.jsonl')
             if r.get('event') == 'exit']
    assert len(exits) == 40, len(exits)
    groups = defaultdict(list)
    for row in exits:
        assert set(row['samples']) == {'3', '30'}, row
        expected = '0' if row['keep_awake'] else '1'
        assert all(value == expected for value in row['samples'].values()), row
        assert "showing app '" not in row['after'], row
        if row['action'] != 'screen-off':
            assert 'showing Start' in row['after'], row
        else:
            assert 'Waydroid screen-off policy completed' in row['after'], row
        groups[('on' if row['keep_awake'] else 'off') + '/' + row['action']].append(row['samples'])
    assert len(groups) == 8, groups
    assert all(len(group) == 5 for group in groups.values()), groups
    launches = defaultdict(list)
    observed = defaultdict(list)
    current = None
    for row in records(args.results / 'reliability-final-launches.jsonl'):
        if row.get('event') == 'launch-state':
            assert current is None, row
            current = row['state']
            assert row['freeze'] == {'stopped': None, 'frozen': '1', 'running': '0', 'restart': '1'}[current], row
            assert ('STOPPED' if current == 'stopped' else 'RUNNING') in row['status'], row
        elif row.get('event') == 'tap':
            assert current is not None, row
            assert row['ack'] == 'com.whatsapp' and row['freeze'] == '0', row
            starts = [line for line in row['compositor'] if "launched 'ztos-android launch com.whatsapp --runtime waydroid'" in line]
            maps = [line for line in row['compositor'] if "showing app 'waydroid.com.whatsapp'" in line]
            assert len(starts) == len(maps) == 1, row
            elapsed = compositor_time(maps[0]) - compositor_time(starts[0])
            assert 0 < elapsed < 100, row
            launches[current].append(round(elapsed, 3))
            observed[current].append(round(row['seconds_observed'], 3))
            current = None
    assert current is None
    assert set(launches) == {'stopped', 'frozen', 'running', 'restart'}, launches
    assert all(len(group) == 5 for group in launches.values()), launches
    sections = re.split(r'\n--- ', (args.results / 'reliability-final-launch.log').read_text())
    sections = [section for section in sections if re.match(r'\d{4}-.* launch com\.whatsapp tty=', section)][-20:]
    assert len(sections) == 20
    for section in sections:
        assert section.count('rm -f /run/user/1000/ztos-waydroid-visible') == 1
        assert 'No visible Waydroid window' not in section
        assert 'exit=0' in section
    preflights = sum('Wayland compositor changed; replacing stale Android session before launch.' in s for s in sections)
    assert preflights == 5, preflights
    print(json.dumps({'exit_trials': 40, 'freeze_samples': 80, 'exits': groups,
                      'launch_trials': 20, 'retries': 0, 'compositor_preflights': preflights,
                      'tap_to_map_seconds': {state: {'runs': values, 'min': min(values),
                           'median': statistics.median(values), 'max': max(values)}
                         for state, values in observed.items()},
                      'launch_seconds': {state: {'runs': values, 'min': min(values),
                           'median': statistics.median(values), 'max': max(values)}
                         for state, values in launches.items()}}, indent=2))


if __name__ == '__main__':
    main()
