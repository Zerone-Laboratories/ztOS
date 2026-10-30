#!/usr/bin/env python3
# r2-reader.py SECONDS OUT: hold a gate reader; one line a GGA: boottime and fix quality.
# Never positions.
import socket, sys, time
dur = float(sys.argv[1]); out = open(sys.argv[2], 'w', buffering=1)
bt = lambda: time.clock_gettime(time.CLOCK_BOOTTIME)
t0 = bt(); buf = b''
try:
    s = socket.create_connection(('127.0.0.1', 10110), timeout=5); s.settimeout(2)
    out.write(f'{bt():.2f} connected\n')
    while bt() - t0 < dur:
        try:
            d = s.recv(4096)
        except socket.timeout:
            continue
        if not d:
            out.write(f'{bt():.2f} closed\n'); break
        buf += d
        while b'\n' in buf:
            l, buf = buf.split(b'\n', 1); f = l.decode('ascii', 'replace').strip().split(',')
            if f[0][3:6] == 'GGA' and len(f) > 6:
                out.write(f'{bt():.2f} q={f[6] or 0}\n')
except OSError as e:
    out.write(f'{bt():.2f} error {e}\n')
