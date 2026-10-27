#!/usr/bin/env python3
"""Record ten seconds through PulseAudio; retain WAV, pw-top and level/gap metrics."""
import argparse
import array
import json
import math
import os
from pathlib import Path
import subprocess
import time
import wave

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('prefix')
p.add_argument('--source', help='default PulseAudio source when omitted')
p.add_argument('--tone', action='store_true', help='play a quiet 1 kHz reference through the default speaker')
a = p.parse_args()
os.environ.setdefault('XDG_RUNTIME_DIR', '/run/user/1000')
base = Path(a.prefix)
players = []
if a.tone:
    tone = str(base) + '-tone.wav'
    with wave.open(tone, 'wb') as w:
        w.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
        w.writeframes(array.array('h', (int(32767 * 0.03 * math.sin(2 * math.pi * 1000 * i / 48000)) for i in range(14 * 48000))).tobytes())
    players.append(subprocess.Popen(['paplay', tone]))
    time.sleep(1)
with open(str(base) + '-top.txt', 'w') as log, open(str(base) + '.raw', 'wb') as raw:
    top = subprocess.Popen(['pw-top', '-b'], stdout=log)
    command = ['parec', '--raw', '--format=s16le', '--rate=48000', '--channels=1', '--latency-msec=100']
    if a.source:
        command.append('--device=' + a.source)
    rec = subprocess.Popen(command, stdout=raw)
    time.sleep(11)
    for proc in [rec, top] + players:
        proc.terminate()
    for proc in [rec, top] + players:
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
data = Path(str(base) + '.raw').read_bytes()
# Discard startup; retain the last ten seconds (report if delivery was short).
data = data[-48000 * 10 * 2:]
if len(data) < 48000 * 10 * 2:
    raise SystemExit(f'Capture too short: {len(data)/96000:.3f} seconds; inspect raw data and stderr')
x = array.array('h', data)
with wave.open(str(base) + '.wav', 'wb') as w:
    w.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
    w.writeframes(data)
def db(value):
    return 20 * math.log10(max(value / 32768, 1e-12))
run = longest = gaps = 0
for value in x:
    if value == 0:
        run += 1
        longest = max(longest, run)
    else:
        gaps += run >= 48
        run = 0
gaps += run >= 48
result = dict(
    seconds=len(x) / 48000,
    rms_dbfs=db(math.sqrt(sum(v * v for v in x) / len(x))),
    peak_dbfs=db(max(map(abs, x))),
    zero_runs_ge_1ms=gaps,
    longest_zero_ms=longest / 48,
    jumps_gt_half_scale=sum(abs(v - u) > 16384 for u, v in zip(x, x[1:])),
)
# pw-top's initial C rows contain placeholder zero counters. Use active rows only.
errors = {}
for line in Path(str(base) + '-top.txt').read_text().splitlines():
    fields = line.split()
    if len(fields) < 10 or fields[0] != 'R':
        continue
    name = fields[-1]
    try:
        count = int(fields[8])
    except ValueError:
        continue
    errors.setdefault(name, []).append(count)
result['active_error_counters'] = {
    name: dict(first=values[0], last=values[-1], delta=values[-1]-values[0])
    for name, values in errors.items()
}
Path(str(base) + '.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result))
