#!/usr/bin/env python3
# r2-fix.py FILE WOKE: from r2-reader output, the longest gap, and the first fix after WOKE (boottime)
import sys
rows = []
connected = None
for l in open(sys.argv[1]):
    p = l.split()
    if len(p) == 2 and p[1] == 'connected' and connected is None:
        connected = float(p[0])
    if len(p) == 2 and p[1].startswith('q='):
        rows.append((float(p[0]), p[1][2:] not in ('', '0')))
woke = float(sys.argv[2])
if woke == 0 and connected is not None:
    woke = connected	# off mode: from the reader's connect
gap, at = 0, None
for (a, _), (b, _) in zip(rows, rows[1:]):
    if b - a > gap: gap, at = b - a, a
fix = next((t for t, ok in rows if t >= woke and ok), None)
nofix_after = [t for t, ok in rows if t >= woke and not ok]
print(f'gga={len(rows)} longest_gap={gap:.1f}s at={at} first_fix_after_return={None if fix is None else round(fix - woke, 1)}s nofix_lines_after={len(nofix_after)}')
