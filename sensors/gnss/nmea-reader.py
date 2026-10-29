#!/usr/bin/env python3
"""Hold a gate reader open for N s; report fix gaps and constellation counts.
No positions are printed or stored: only times, counts, PRNs, SNR, accuracy."""
import collections, socket, sys, time
dur = float(sys.argv[1]) if len(sys.argv) > 1 else 300
s = socket.create_connection(("127.0.0.1", 10110))
s.settimeout(1)
t0 = time.time()
buf = b""
last_fix = None
gsv = collections.Counter()
prns = collections.defaultdict(set)
snr = collections.defaultdict(list)
quality = collections.Counter()
gsa = collections.Counter()
tfx = []
hd = []
bad = 0
def ck(line):
    if not line.startswith("$") or "*" not in line: return False
    b, _, c = line[1:].partition("*")
    x = 0
    for ch in b.encode(): x ^= ch
    return "%02X" % x == c[:2].upper()
def out(m): print("%7.1f %s" % (time.time() - t0, m), flush=True)
out("connected")
while time.time() - t0 < dur:
    try:
        d = s.recv(4096)
    except socket.timeout:
        d = None
    if d == b"":
        out("gate closed"); break
    if d:
        buf += d
    *lines, buf = buf.split(b"\n")
    for raw in lines:
        l = raw.decode("ascii", "replace").strip()
        if not l: continue
        if not ck(l): bad += 1; continue
        f = l[1:l.index("*")].split(",")
        k = f[0]
        if k.endswith("GSV") and len(f) > 3:
            gsv[k[:2]] += 1
            for i in range(4, len(f) - 3, 4):
                if f[i]:
                    prns[k[:2]].add(int(f[i]))
                    if f[i + 3]: snr[k[:2]].append(int(f[i + 3]))
        elif k.endswith("GSA"):
            gsa[k[:2]] += 1
        elif k.endswith("GGA") and len(f) > 8:
            quality[f[6]] += 1
            if f[6] not in ("", "0"):
                if f[8]: hd.append(float(f[8]) * 4)
                now = time.time()
                if last_fix is None or now - last_fix > 3:
                    out("fix (gap %.1f s)" % (now - last_fix) if last_fix else "first fix")
                last_fix = now
        elif k == "PZTFX" and len(f) > 1 and f[1]:
            tfx.append(float(f[1]))
        elif k == "PZTLF":
            out("PZTLF hello received")
def med(v): v = sorted(v); return v[len(v) // 2] if v else None
out("GSV sentences by talker: %s" % dict(gsv))
out("GSA sentences by talker: %s" % dict(gsa))
for t in sorted(prns):
    out("%s PRNs seen: %s; SNR>0 median %s" % (t, sorted(prns[t]), med([x for x in snr[t] if x > 0])))
out("GGA fix quality counts: %s" % dict(quality))
out("PZTFX count %d median %s m; HDOP*4 count %d median %s m; bad checksums %d" % (len(tfx), med(tfx), len(hd), med(hd), bad))
