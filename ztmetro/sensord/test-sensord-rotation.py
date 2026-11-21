#!/usr/bin/env python3
"""ztoscsf-sensord's auto rotation, on the host.

Part 1 feeds synthetic accelerometer traces through the decision alone
(`--rotation-selftest TRACE [CONF]`) and checks every decision and its timing.
Part 2 runs the daemon against a pty hub, as test-sensord-queue.py does, and
checks the hub traffic rotation causes (one transaction at a time, its own
want flag and period beside a watch), the shell's messages, the files and the
state."""
import math
import os
import pty
import random
import select
import signal
import struct
import subprocess
import sys
import tempfile
import time
import tty
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE / 'ztoscsf-sensord.c'
G = 4096		# raw counts per g (the daemon's lsb_per_g default)
DT = 40			# ms between samples (period_ms)
failures = []


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        failures.append(what)


# --- traces -------------------------------------------------------------

def vec(angle, tilt):
    """the reading, in g, with the edge `angle` up (0 top, 90 right) and the
    screen tilted back by `tilt` (+90 face up)"""
    a, t = math.radians(angle), math.radians(tilt)
    return [math.cos(t) * math.sin(a), math.cos(t) * math.cos(a), math.sin(t)]


class Trace:
    def __init__(self, seed=1):
        self.t = 0
        self.lines = []
        self.rand = random.Random(seed)

    def sample(self, v):
        self.lines.append('%d %d %d %d' % (self.t, round(v[0] * G), round(v[1] * G), round(v[2] * G)))
        self.t += DT

    def hold(self, angle, tilt, ms, noise=None, jitter=0.0):
        for _ in range(ms // DT):
            v = vec(angle, tilt)
            if noise:
                n = noise(self.t)
                v = [v[i] + n[i] for i in range(3)]
            if jitter:
                v = [x + self.rand.uniform(-jitter, jitter) for x in v]
            self.sample(v)

    def ramp(self, a0, t0, a1, t1, ms):
        """from (a0, t0) to (a1, t1) in ms, ending exactly at (a1, t1)"""
        n = ms // DT
        for i in range(1, n + 1):
            f = i / n
            self.sample(vec(a0 + (a1 - a0) * f, t0 + (t1 - t0) * f))
        return self.t - DT	# the time of the last ramp sample

    def gap(self, ms):
        self.t += ms - DT

    def raw(self, line):
        self.lines.append(line)

    def text(self):
        return '\n'.join(self.lines) + '\n'


def run_trace(exe, d, name, trace, conf=None, samples=False):
    path = d / (name + '.trace')
    path.write_text(trace.text())
    args = [str(exe), '--rotation-selftest', str(path)] + ([str(conf)] if conf else [])
    env = dict(os.environ, ZTOSCSF_SELFTEST_SAMPLES='1') if samples else None
    r = subprocess.run(args, capture_output=True, text=True, env=env)
    events = []
    for line in r.stdout.splitlines():
        f = line.split()
        events.append((int(f[0]), f[1], f[2:], line))
    return events, r.stderr


def decisions(events):
    """[(t, orientation, instant)]"""
    return [(t, int(rest[0]), len(rest) > 1 and rest[1] == 'instant')
            for t, kind, rest, _ in events if kind == 'orientation']


def show(name, events):
    for e in events:
        print('       %s: %s' % (name, e[3]))


def selftests(exe, d):
    # clean turns, both ways: one commit, within about 250 ms of the turn's end
    for to, name in ((90, 'right edge up'), (270, 'left edge up')):
        tr = Trace()
        tr.hold(0, 30, 1000)
        end = tr.ramp(0, 30, to if to == 90 else -90, 30, 300)
        tr.hold(to, 30, 1500)
        ev, _ = run_trace(exe, d, 'turn%d' % to, tr)
        dec = decisions(ev)
        show('turn %d' % to, ev)
        check(len(dec) == 2 and dec[0][1:] == (0, True) and dec[0][0] <= 160,
              'turn %d: first "0 instant" within 160 ms of the first sample' % to)
        check(len(dec) == 2 and dec[1][1:] == (to, False) and 0 < dec[1][0] - end <= 250,
              'turn %d (%s): one commit "%d", %s ms after the turn ends (<= 250)'
              % (to, name, to, dec[1][0] - end if len(dec) == 2 else '?'))

    # a turn and back
    tr = Trace()
    tr.hold(0, 30, 1000)
    tr.ramp(0, 30, 90, 30, 300)
    tr.hold(90, 30, 1000)
    end = tr.ramp(90, 30, 0, 30, 300)
    tr.hold(0, 30, 1500)
    ev, _ = run_trace(exe, d, 'turnback', tr)
    dec = decisions(ev)
    show('turn and back', ev)
    check([x[1:] for x in dec] == [(0, True), (90, False), (0, False)] and 0 < dec[2][0] - end <= 250,
          'turn and back: 0 instant, 90, 0 (back %s ms after the return ends)'
          % (dec[2][0] - end if len(dec) == 3 else '?'))

    # a 40-degree wobble each way and back: nothing
    for w in (40, -40):
        tr = Trace()
        tr.hold(0, 30, 1000)
        tr.ramp(0, 30, w, 30, 300)
        tr.hold(w, 30, 1500)
        tr.ramp(w, 30, 0, 30, 300)
        tr.hold(0, 30, 1000)
        ev, _ = run_trace(exe, d, 'wobble%d' % w, tr)
        dec = decisions(ev)
        check([x[1:] for x in dec] == [(0, True)], 'wobble %+d degrees, held 1.5 s, and back: no commit' % w)

    # walking: +-0.3 g at 2 Hz on every axis (phases apart), in portrait and landscape
    def walk(t):
        w = 2 * math.pi * 2 * t / 1000
        return [0.3 * math.sin(w + 1.0), 0.3 * math.sin(w), 0.3 * math.sin(w + 2.0)]
    for orient in (0, 90):
        tr = Trace(seed=orient + 7)
        tr.hold(orient, 30, 8000, noise=walk, jitter=0.03)
        ev, _ = run_trace(exe, d, 'walk%d' % orient, tr)
        dec = decisions(ev)
        check([x[1:] for x in dec] == [(orient, True)],
              'walking +-0.3 g at 2 Hz for 8 s in %s: no commit after the first' %
              ('portrait' if orient == 0 else 'landscape'))

    # laid flat from portrait and from landscape: nothing, the orientation stays;
    # the reasons are logged once per change, not per sample
    for orient in (0, 90):
        tr = Trace(seed=orient + 11)
        tr.hold(orient, 30, 1000)
        flat_from = tr.ramp(orient, 30, orient, 88, 600)
        tr.hold(orient, 89, 4000, jitter=0.03)
        ev, _ = run_trace(exe, d, 'layflat%d' % orient, tr)
        dec = decisions(ev)
        show('lay flat %d' % orient, ev)
        notes = [e for e in ev if e[1] == 'candidate' and e[0] > flat_from + 1000]
        check([x[1:] for x in dec] == [(orient, True)], 'laid flat from %d: no commit' % orient)
        check(len(notes) <= 1, 'laid flat from %d: %d reason line(s) over 3 s of flat (once per change)'
              % (orient, len(notes)))

    # picked up from flat into landscape: commits, after the after-flat hold
    tr = Trace(seed=3)
    tr.hold(0, 30, 1000)
    tr.ramp(0, 30, 0, 89, 400)
    tr.hold(0, 89, 2000, jitter=0.02)
    lift = tr.t
    tr.ramp(90, 89, 90, 30, 400)
    tr.hold(90, 30, 2000)
    ev, _ = run_trace(exe, d, 'pickup', tr)
    dec = decisions(ev)
    show('pick up', ev)
    after_flat = any(e[1] == 'candidate' and 'after-flat' in e[3] for e in ev)
    check([x[1:] for x in dec] == [(0, True), (90, False)] and 500 <= dec[1][0] - lift <= 1000 and after_flat,
          'picked up from flat into landscape: "90" %s ms after the lift starts (after-flat hold, 500..1000)'
          % (dec[1][0] - lift if len(dec) == 2 else '?'))
    # the same, with the screen coming on while it lies flat: nothing until the lift
    tr = Trace(seed=4)
    tr.hold(0, 89, 2000, jitter=0.02)
    lift = tr.t
    tr.ramp(90, 89, 90, 30, 400)
    tr.hold(90, 30, 2000)
    ev, _ = run_trace(exe, d, 'pickup-first', tr)
    dec = decisions(ev)
    check([x[1:] for x in dec] == [(90, True)] and dec[0][0] - lift >= 500,
          'screen on while flat: nothing while flat, then "90 instant" after the lift and the hold')

    # face down: tipped forward from portrait, and flipped over sideways from flat
    tr = Trace(seed=5)
    tr.hold(0, 30, 1000)
    tr.ramp(0, 30, 0, -88, 500)
    tr.hold(0, -89, 3000, jitter=0.02)
    ev, _ = run_trace(exe, d, 'facedown', tr)
    check([x[1:] for x in decisions(ev)] == [(0, True)], 'face down, tipped from portrait: no commit')
    tr = Trace(seed=6)
    tr.hold(0, 30, 1000)
    tr.ramp(0, 30, 0, 89, 400)
    tr.hold(0, 89, 1500, jitter=0.02)
    for i in range(1, 11):			# rolled over the right edge in 400 ms
        p = math.radians(180 * i / 10)
        tr.sample([math.sin(p), 0.0, math.cos(p)])
    tr.hold(0, -89, 2000, jitter=0.02)
    ev, _ = run_trace(exe, d, 'flipdown', tr)
    show('flip face down', ev)
    check([x[1:] for x in decisions(ev)] == [(0, True)],
          'face down, rolled over sideways from flat (through landscape): no commit')

    # overhead: below -40 it holds until above -15, though -20 is inside the tolerance
    tr = Trace()
    tr.hold(0, 30, 1000)
    tr.ramp(0, 30, 90, -60, 400)
    tr.hold(90, -60, 1500)
    tr.ramp(90, -60, 90, -20, 300)
    tr.hold(90, -20, 1500)
    out = tr.t
    tr.ramp(90, -20, 90, -10, 200)
    tr.hold(90, -10, 1500)
    ev, _ = run_trace(exe, d, 'overhead', tr)
    dec = decisions(ev)
    show('overhead', ev)
    check([x[1:] for x in dec] == [(0, True), (90, False)] and dec[1][0] > out,
          'overhead at -60 then -20: no commit; above -15: "90" (%s ms after leaving -20)'
          % (dec[1][0] - out if len(dec) == 2 else '?'))
    tr = Trace()
    tr.hold(0, 30, 1000)
    tr.ramp(0, 30, 90, -20, 400)
    tr.hold(90, -20, 1500)
    ev, _ = run_trace(exe, d, 'tilt-20', tr)
    check([x[1:] for x in decisions(ev)] == [(0, True), (90, False)],
          'control: straight to landscape at -20 (never overhead) commits "90"')

    # shaken at +-1.5 g (3 Hz, along gravity, +-0.5 g across) in landscape: nothing
    # during, nothing until accel_hold_ms (500) after the filtered |a| was last
    # outside mag_lo..mag_hi
    # (a) a fast shake, 3 Hz, +-1.5 g across the phone (it swings the raw angle by
    # far more than 45 degrees), in portrait and in landscape: no commit at all
    for orient in (0, 90):
        tr = Trace()
        tr.hold(orient, 30, 1000)
        t0 = tr.t
        across = [math.cos(math.radians(orient)), -math.sin(math.radians(orient))]

        def fast(t):
            w = 2 * math.pi * 3 * (t - t0) / 1000
            s = 1.5 * math.sin(w)
            return [s * across[0], s * across[1], 0.5 * math.sin(w + 1.3)]
        tr.hold(orient, 30, 3000, noise=fast)
        tr.hold(orient, 30, 1500)
        ev, _ = run_trace(exe, d, 'shakefast%d' % orient, tr)
        check([x[1:] for x in decisions(ev)] == [(orient, True)],
              'shaken at +-1.5 g, 3 Hz, across the phone for 3 s in %d: no commit' % orient)
    # (b) turned to landscape while shaken at +-1.5 g, 1.5 Hz, along its length:
    # no commit while shaken; "90" only accel_hold_ms after the filtered |a| was
    # last outside mag_lo..mag_hi
    tr = Trace()
    tr.hold(0, 30, 1000)
    t0 = tr.t

    def slow(t):
        w = 2 * math.pi * 1.5 * (t - t0) / 1000
        return [1.5 * math.sin(w), 0.3 * math.sin(w + 1.3), 0.3 * math.sin(w + 2.1)]
    for i in range(1, 9):			# the turn, 320 ms, already shaken
        v = vec(90 * i / 8, 30)
        n = slow(tr.t)
        tr.sample([v[k] + n[k] for k in range(3)])
    tr.hold(90, 30, 2000, noise=slow)
    still = tr.t
    tr.hold(90, 30, 2000)
    ev, _ = run_trace(exe, d, 'shakeslow', tr, samples=True)
    dec = decisions(ev)
    out = [t for t, kind, rest, _ in ev if kind == 'sample' and not 0.59 <= float(rest[0]) <= 1.41]
    last = max(out) if out else 0
    blocked = [e for e in ev if e[1] == 'sample' and e[2][-1] == 'accelerating']
    check([x[1:] for x in dec] == [(0, True), (90, False)] and dec[1][0] >= still
          and dec[1][0] - last >= 500 and len(out) >= 6 and blocked,
          'shaken at +-1.5 g, 1.5 Hz, while turned: no commit while shaken (|a| out %d times); '
          '"90" %s ms after the last out-of-range |a| (>= 500), %s ms after the shaking stops'
          % (len(out), dec[1][0] - last if len(dec) == 2 else '?', dec[1][0] - still if len(dec) == 2 else '?'))

    # a gap over max_gap_ms: the filter starts again (the orientation stands)
    tr = Trace()
    tr.hold(0, 30, 1000)
    tr.gap(1500)
    back = tr.t
    tr.hold(90, 30, 1000)
    ev, _ = run_trace(exe, d, 'gap', tr)
    dec = decisions(ev)
    show('gap', ev)
    resets = [e[0] for e in ev if e[1] == 'reset']
    check(resets == [back], 'a 1.5 s gap: "reset gap" at the first sample after it')
    check([x[1:] for x in dec] == [(0, True), (90, False)] and dec[1][0] - back <= 200,
          'after the gap: "90", not instant, %s ms after the samples resume'
          % (dec[1][0] - back if len(dec) == 2 else '?'))
    tr = Trace()
    tr.hold(0, 30, 1000)
    tr.gap(900)
    tr.hold(0, 30, 500)
    ev, _ = run_trace(exe, d, 'gap900', tr)
    check(not [e for e in ev if e[1] == 'reset'], 'a 0.9 s gap: no reset')
    tr = Trace()
    tr.hold(0, 30, 400)
    tr.raw('%d 0 0 0' % tr.t)
    tr.t += DT
    tr.hold(0, 30, 400)
    ev, _ = run_trace(exe, d, 'zero', tr)
    check([e[0] for e in ev if e[1] == 'reset'] == [400] and len(decisions(ev)) == 1,
          'an all-zero reading: "reset zero", the orientation stands')

    # upside down
    tr = Trace()
    tr.hold(180, 30, 3000)
    ev, _ = run_trace(exe, d, 'upside', tr)
    check(decisions(ev) == [], 'upside down from the start, allow_180 = 0: nothing')
    tr = Trace()
    tr.hold(0, 30, 1000)
    for i in range(1, 13):			# tumbled over the top edge, through face up
        p = math.radians(30 + 120 * i / 12)
        tr.sample([0.0, math.cos(p), math.sin(p)])
    tr.hold(180, 30, 2000)
    ev, _ = run_trace(exe, d, 'tumble', tr)
    check([x[1:] for x in decisions(ev)] == [(0, True)], 'tumbled from portrait to upside down: no commit')
    tr = Trace()
    tr.raw('set allow_180 = 1')
    tr.hold(180, 30, 1000)
    ev, _ = run_trace(exe, d, 'upside-allowed', tr)
    check([x[1:] for x in decisions(ev)] == [(180, True)], 'upside down with allow_180 = 1: "180 instant"')

    # the screen coming on while in landscape: the first decision is instant
    tr = Trace()
    tr.hold(90, 30, 1000)
    tr.raw('reset')
    second = tr.t
    tr.hold(90, 30, 1000)
    tr.raw('reset')
    third = tr.t
    tr.hold(0, 30, 1000)
    ev, _ = run_trace(exe, d, 'screenon', tr)
    dec = decisions(ev)
    show('screen on', ev)
    check(len(dec) == 3 and dec[0][1:] == (90, True) and dec[0][0] <= 160
          and dec[1][1:] == (90, True) and dec[1][0] - second <= 160
          and dec[2][1:] == (0, True) and dec[2][0] - third <= 160,
          'screen on in landscape: "90 instant" within 160 ms; again after a reset; then "0 instant"')

    # rotation.conf: axes, unknown keys, bad values, a missing file
    conf = d / 'rotation.conf'
    conf.write_text('# test\naxes = "-y x z"\nfoo = 1\ntau_ms = abc\nsettle_ms = 80   # inline\n'
                    'tilt_tol_90 = -25 65\nno equals here\n')
    tr = Trace()
    tr.raw('0 0 -4096 0')			# raw -y is the device's +x: right edge up
    for i in range(1, 6):
        tr.raw('%d 0 -4096 0' % (i * DT))
    ev, err = run_trace(exe, d, 'axes', tr, conf)
    print('       ' + err.strip().replace('\n', '\n       '))
    check([x[1:] for x in decisions(ev)] == [(90, True)], 'axes = "-y x z": raw -y reads as right edge up')
    check(err.count('unknown key "foo"') == 1 and err.count('tau_ms = abc') == 1
          and err.count('not "key = value"') == 1 and len(err.strip().splitlines()) == 3,
          'unknown key, bad value, malformed line: one log line each, nothing else')
    ev, err = run_trace(exe, d, 'axes', tr, d / 'missing.conf')
    # with the default axes raw -y is the bottom edge up: upside down, not taken
    check(err.count('rotation uses its defaults') == 1 and decisions(ev) == [],
          'missing rotation.conf: one log line, the defaults (axes "x y z")')


# --- the daemon on a pty hub --------------------------------------------

class Hub:
    """As in test-sensord-queue.py: sensord's writes as tokens."""
    def __init__(self, master):
        self.fd = master
        self.buf = b''
        self.want = 0

    def _fill(self, until):
        left = until - time.monotonic()
        if left <= 0:
            return False
        r, _, _ = select.select([self.fd], [], [], left)
        if not r:
            return False
        self.buf += os.read(self.fd, 4096)
        return True

    def token(self, timeout):
        until = time.monotonic() + timeout
        while True:
            if self.want and len(self.buf) >= self.want:
                n, self.want = self.want, 0
                out, self.buf = self.buf[:n], self.buf[n:]
                return ('payload', out, time.monotonic())
            if not self.want and self.buf:
                n = 17 if self.buf[0] == 0xa7 else 9
                if len(self.buf) >= n:
                    out, self.buf = self.buf[:n], self.buf[n:]
                    return ('time' if n == 17 else 'header', out, time.monotonic())
            if not self._fill(until):
                return None

    def next(self, timeout=1.5):
        """the next header or payload; the time messages are let pass"""
        until = time.monotonic() + timeout
        while True:
            t = self.token(max(0.0, until - time.monotonic()))
            if t is None or t[0] != 'time':
                return t

    def echo(self, head):
        opts = struct.unpack_from('<H', head, 3)[0]
        length = struct.unpack_from('<H', head, 1)[0]
        self.want = length
        os.write(self.fd, struct.pack('<HHH', 4, opts, length))

    def frame(self, data):
        os.write(self.fd, struct.pack('<HHH', 4 + len(data), 2, len(data)) + data)

    def accel(self, x, y, z):
        self.frame(struct.pack('<BB3hQ', 0x37, 0, x, y, z, 0))

    def txn(self, cmd, timeout=1.5):
        """a header of cmd, echoed: its payload (or None)"""
        t = self.next(timeout)
        if not t or t[0] != 'header' or t[1][0] != cmd:
            return None
        self.echo(t[1])
        p = self.next(1)
        return p[1] if p and p[0] == 'payload' else None


def add(payload):
    return payload and len(payload) == 10 and (payload[0], struct.unpack_from('<i', payload, 1)[0])


def daemon_tests(exe, d):
    root = d / 'root'
    for sub in ('run/user/1000', 'var/lib'):
        (root / sub).mkdir(parents=True)
    (root / 'run/zrnperfd.status').write_text('screen=on\n')
    shell_path = root / 'run/user/1000/ztmetro.ctl'
    os.mkfifo(shell_path)
    shell_fd = os.open(shell_path, os.O_RDONLY | os.O_NONBLOCK)
    conf = d / 'daemon-rotation.conf'
    conf.write_text('period_ms = 40\n')
    seq = d / 'resume_seq'
    seq.write_text('0\n')
    out = root / 'run/ztoscsf-sensors'
    ctl = root / 'run/ztoscsf-sensord.ctl'

    def start():
        master, slave = pty.openpty()
        tty.setraw(slave)
        tty.setraw(master)
        env = dict(os.environ, ZTOSCSF_SSP=os.ttyname(slave), ZTOSCSF_RESUME_SEQ=str(seq),
                   ZTOSCSF_ROOT=str(root), ZTOSCSF_ROTATION_CONF=str(conf))
        err = open(d / 'daemon.stderr', 'a')
        proc = subprocess.Popen([str(exe)], env=env, stderr=err, stdout=err)
        return proc, Hub(master), err

    def say(line):
        fd = os.open(ctl, os.O_WRONLY)
        os.write(fd, (line + '\n').encode())
        os.close(fd)

    def shell_lines():
        try:
            return os.read(shell_fd, 4096).decode().splitlines()
        except BlockingIOError:
            return []

    def feed(v, n, every=0.04):
        for _ in range(n):
            hub.accel(*v)
            time.sleep(every)

    def quiet(timeout=0.4):
        t = hub.next(timeout)
        return t is None

    landscape = (round(G * math.cos(math.radians(30))), 0, round(G * 0.5))
    portrait = (0, round(G * math.cos(math.radians(30))), round(G * 0.5))

    proc, hub, err = start()
    try:
        time.sleep(0.2)
        check(add(hub.txn(0xa1, 3)) == (9, 1000), 'daemon: start, ADD light; rotation off by default: nothing more')
        check(quiet(), 'daemon: no ADD accel while rotation is off')
        check('rotation=off' in (out / 'status').read_text(), 'daemon: status has rotation=off')

        say('rotation log on')
        say('rotation on')
        check(add(hub.txn(0xa1)) == (0, 40), 'rotation on: ADD accel at 40 ms')
        check('rotation=on' in (out / 'status').read_text(), 'rotation on: status rotation=on')
        check('rotation on' in (root / 'var/lib/ztoscsf-sensord/state').read_text(),
              'rotation on: remembered in the state file')
        feed(landscape, 12)
        time.sleep(0.1)
        lines = shell_lines()
        check(lines == ['orientation 90 instant'], 'rotation on in landscape: the shell gets "orientation 90 instant": %s' % lines)
        check((out / 'orientation').read_text() == '90\n', 'orientation file: 90')
        check(not (out / 'accel').exists(), 'rotation alone writes no accel file')

        say('watch accel')			# the watch's 200 ms is slower: nothing changes
        feed(landscape, 3)
        check(quiet(), 'watch accel (200 ms) beside rotation (40 ms): no hub transaction')
        check((out / 'accel').read_text().split() == [str(x) for x in landscape],
              'the watcher sees the readings (accel file)')

        say('watch accel 10')			# faster: REMOVE, then ADD at 10, one at a time
        t = hub.next()
        check(t and t[0] == 'header' and t[1][0] == 0xa2, 'watch accel 10: REMOVE accel header')
        check(quiet(0.3), 'watch accel 10: no ADD before the REMOVE is echoed')
        hub.echo(t[1])
        p = hub.next(1)
        check(p and p[0] == 'payload' and len(p[1]) == 5 and p[1][0] == 0, 'watch accel 10: REMOVE payload')
        check(add(hub.txn(0xa1)) == (0, 10), 'watch accel 10: then ADD accel at 10 ms')

        # the watch runs out (5 s, checked as frames arrive): back to rotation's 40, never off
        end = time.monotonic() + 7
        got = []
        while time.monotonic() < end and len(got) < 2:
            hub.accel(*portrait)
            t = hub.next(0.2)
            if t and t[0] == 'header':
                got.append(t[1][0])
                hub.echo(t[1])
                p = hub.next(1)
                got.append(p[1] if p else None)
                if t[1][0] == 0xa2:
                    got = [0xa2]
                    t = hub.next(1)
                    if t and t[0] == 'header':
                        hub.echo(t[1])
                        p = hub.next(1)
                        got = [0xa2, t[1][0], p[1] if p else None]
                        break
        check(len(got) == 3 and got[1] == 0xa1 and add(got[2]) == (0, 40),
              'the watch runs out: REMOVE, then ADD accel at 40 (rotation goes on)')
        before = (out / 'accel').read_text()
        hub.accel(1, 2, 3)
        time.sleep(0.1)
        check((out / 'accel').read_text() == before, 'after the watch: no accel file updates from rotation')
        feed(portrait, 12)
        time.sleep(0.1)
        lines = shell_lines()
        check('orientation 0' in lines, 'rotation still decides after the watch ran out: %s' % lines)

        # the hub restarts: everything again, one at a time; the orientation stands
        hub.frame(bytes([0x0a]))
        check(add(hub.txn(0xa1)) == (9, 1000), 'hub restart: ADD light')
        check(add(hub.txn(0xa1)) == (0, 40), 'hub restart: ADD accel at 40')
        feed(portrait, 12)
        time.sleep(0.1)
        check(shell_lines() == [], 'hub restart: the filter starts again, no new message for the same orientation')

        say('rotation off')
        check(hub.txn(0xa2) == bytes([0, 0, 0, 0, 0]), 'rotation off: REMOVE accel')
        check(quiet(), 'rotation off: nothing else')
        check(not (out / 'orientation').exists(), 'rotation off: orientation file removed')
        check('rotation off' in (root / 'var/lib/ztoscsf-sensord/state').read_text(), 'rotation off: remembered')

        say('rotation record 1')		# a record streams with rotation off, tells the shell nothing
        check(add(hub.txn(0xa1)) == (0, 40), 'rotation record 1: ADD accel at 40')
        feed(landscape, 30)
        t = hub.next(1)
        check(t and t[0] == 'header' and t[1][0] == 0xa2, 'record done after 1 s: REMOVE accel')
        if t:
            hub.echo(t[1])
            hub.next(1)
        trace = (out / 'rotation.trace').read_text().splitlines()
        check(len(trace) > 20 and trace[0].startswith('#') and any('orientation 90 instant' in x for x in trace),
              'rotation.trace: %d lines, with the decision in it' % len(trace))
        check(shell_lines() == [], 'record with rotation off: nothing to the shell')
        ev = subprocess.run([str(exe), '--rotation-selftest', str(out / 'rotation.trace')],
                            capture_output=True, text=True).stdout
        check('orientation 90 instant' in ev, 'a rotation.trace replays through --rotation-selftest')

        say('rotation on')
        check(add(hub.txn(0xa1)) == (0, 40), 'rotation on again: ADD accel')
        say('screen off')
        check(hub.txn(0xa2) is not None, 'screen off: REMOVE light')
        check(hub.txn(0xa2) is not None, 'screen off: then REMOVE accel')
        say('screen on')
        check(add(hub.txn(0xa1)) == (9, 1000) and add(hub.txn(0xa1)) == (0, 40), 'screen on: ADD light, ADD accel 40')
        feed(landscape, 12)
        time.sleep(0.1)
        lines = shell_lines()
        check(lines == ['orientation 90 instant'], 'screen on: "orientation 90 instant" again: %s' % lines)

        proc.send_signal(signal.SIGTERM)
        for _ in range(2):
            t = hub.next(1)
            if t and t[0] == 'header':
                hub.echo(t[1])
                hub.next(1)
        proc.wait(3)
        check(proc.returncode == 0, 'stops cleanly')

        # a start with rotation remembered on: light, then accel at 40
        proc, hub, err = start()
        time.sleep(0.2)
        check(add(hub.txn(0xa1, 3)) == (9, 1000) and add(hub.txn(0xa1)) == (0, 40),
              'restart with "rotation on" in the state file: ADD light, ADD accel 40')
        proc.send_signal(signal.SIGTERM)
        for _ in range(2):
            t = hub.next(1)
            if t and t[0] == 'header':
                hub.echo(t[1])
                hub.next(1)
        proc.wait(3)
    finally:
        if proc.poll() is None:
            proc.kill()
        err.close()
        log = (d / 'daemon.stderr').read_text()
        print('       ' + log.strip().replace('\n', '\n       '))
        check('no echo' not in log and 'queue full' not in log, 'daemon: every transaction echoed, none dropped')
        check(log.count('rotation: orientation 90 instant') >= 2, 'rotation log: decisions in the journal')


def main():
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        exe = d / 'sensord'
        build = subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wno-unused-parameter', '-Werror',
                                '-o', str(exe), str(SOURCE), '-lm'], capture_output=True, text=True)
        check(build.returncode == 0, 'builds with -Wall, no warnings ' + build.stderr.strip())
        if build.returncode:
            sys.exit(1)
        selftests(exe, d)
        daemon_tests(exe, d)
    print('PASS' if not failures else f'{len(failures)} FAILED')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
