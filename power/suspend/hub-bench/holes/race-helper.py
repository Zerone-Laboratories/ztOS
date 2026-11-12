#!/usr/bin/env python3
"""R1 bench helper: wait for zrn-sleepd's 'wakeup_count' step, sleep a
seeded 0-800 ms, then make the inhibit timed-r2 (tmp + rename) and say so in
the kernel log. Run as root next to `zrn-sleepd once`."""
import os, random, sys, time
n = int(sys.argv[1])
random.seed(n)
delay = random.uniform(0, float(sys.argv[2]) if len(sys.argv) > 2 else 0.8)
step = '/run/zrn-sleepd/step'
inhibit = '/run/zrn-sleepd/inhibit'
def bt():
    return time.clock_gettime(time.CLOCK_BOOTTIME)
def kmsg(text):
    try:
        with open('/dev/kmsg', 'w') as f:
            f.write(text + '\n')
    except OSError:
        pass
t0 = bt()
print(f'r2 helper {n}: delay {delay * 1000:.0f} ms, waiting', flush=True)
kmsg(f'r2: helper {n} start delay_ms={delay * 1000:.0f}')
while bt() - t0 < 180:
    try:
        if 'wakeup_count' in open(step).read():
            break
    except OSError:
        pass
    time.sleep(0.01)
else:
    print('r2 helper: no wakeup_count step in 180 s', flush=True)
    kmsg(f'r2: helper {n} gave up')
    sys.exit(1)
seen = bt()
kmsg(f'r2: step seen {n}')
time.sleep(delay)
tmp = '/run/zrn-sleepd/.r2.tmp'
with open(tmp, 'w'):
    pass
end = time.time() + 60
os.utime(tmp, (end, end))
os.replace(tmp, os.path.join(inhibit, 'timed-r2'))
done = bt()
kmsg(f'r2: renamed {n} after_step_ms={(done - seen) * 1000:.0f}')
print(f'r2 helper {n}: step seen at {seen:.3f}, renamed at {done:.3f} (boot time)', flush=True)
