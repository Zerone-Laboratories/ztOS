#!/usr/bin/env python3
"""Summarize exported phone evidence without silently discarding failures."""
import collections
import json
import pathlib
import re

ROOT = pathlib.Path(__file__).parent / 'evidence'


def mem(name):
    return {m.group(1): int(m.group(2)) for m in re.finditer(r'^(\w+):\s+(\d+)', (ROOT / name).read_text(), re.M)}


def gems(name):
    result = collections.defaultdict(int)
    for line in (ROOT / name).read_text().splitlines():
        fields = line.split()
        if len(fields) > 7 and fields[1].isdigit():
            result[fields[0]] += int(fields[4])
    return dict(result)


meta = json.loads((ROOT / 'verify-meta.json').read_text())
end = json.loads((ROOT / 'verify-end.json').read_text())
samples = [json.loads(line) for line in (ROOT / 'verify-samples.jsonl').read_text().splitlines()]
probes = [json.loads(line) for line in (ROOT / 'shell-probes-final.jsonl').read_text().splitlines()]
slow = [int(x) for x in re.findall(r'slow frame: (\d+) ms', (ROOT / 'verify-shell.log').read_text())]
errors = []
for line in (ROOT / 'verify-after-dmesg.txt').read_text().splitlines():
    match = re.match(r'\[\s*([0-9.]+)\]', line)
    if match and float(match.group(1)) >= meta['start_uptime']:
        if re.search(r'panfrost_mmu.c:|Failed to get pages|js fault|gpu sched timeout|Unhandled Page fault', line):
            errors.append(line)
summary = {
    'start': meta, 'end': end, 'samples': len(samples), 'kernel_gpu_errors': errors,
    'allocation_failure_lines': sum('Failed to get pages' in x or 'panfrost_mmu.c:' in x for x in errors),
    'gpu_job_fault_events': sum('js fault' in x for x in errors),
    'full_acceptance_passed': not errors and max(slow, default=0) <= 250 and all(p['replied'] and p['latency_ms'] <= 250 for p in probes),
    'shell_pids': sorted({s['shell_pid'] for s in samples}),
    'waydroid_unfrozen_every_sample': all('frozen 0' in s['waydroid_events'] for s in samples),
    'browser_active_every_sample': all(s['browser_active'] == 'active' for s in samples),
    'max_logged_slow_frame_ms': max(slow, default=0),
    'max_memory_sample_response_ms': max(s['state_latency_ms'] for s in samples),
    'continuous_probes': len(probes),
    'probe_max_ms': max(p['latency_ms'] for p in probes),
    'probe_timeouts': sum(not p['replied'] for p in probes),
    'probe_responses_over_250ms': sum(p['latency_ms'] > 250 for p in probes),
    'probe_duration_seconds': probes[-1]['monotonic'] - probes[0]['monotonic'],
    'before_kb': mem('verify-before-meminfo.txt'), 'after_kb': mem('verify-after-meminfo.txt'),
    'min_available_kb': min(s['mem_kb']['MemAvailable'] for s in samples),
    'max_swap_used_kb': max(s['mem_kb']['SwapTotal'] - s['mem_kb']['SwapFree'] for s in samples),
    'zram_last': samples[-1]['zram'],
    'waydroid_first_bytes': int(samples[0]['waydroid_bytes']),
    'waydroid_last_bytes': int(samples[-1]['waydroid_bytes']),
    'gpu_before_bytes': gems('verify-before-gems.txt'),
    'gpu_after_bytes': gems('verify-after-gems.txt'),
}
(ROOT / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
print(json.dumps(summary, indent=2))
