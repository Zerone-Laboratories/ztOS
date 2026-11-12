#!/usr/bin/env python3
"""ztoscsf-sensord's link to the hub, on the host: one transaction in flight,
the echo timeouts, 0xd3 after a resume, and the time message only on an idle
link. The hub is a pty (raw); the resume counter a plain file."""
import os
import pty
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
failures = []


def check(cond, what):
    print(('ok   ' if cond else 'FAIL ') + what)
    if not cond:
        failures.append(what)


class Hub:
    """The far end of the link: reads sensord's writes as tokens (a 9-byte
    header, a 17-byte time message, or the payload an echo asked for)."""
    def __init__(self, master):
        self.fd = master
        self.buf = b''
        self.want = 0		# payload bytes owed after our echo

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
        """(kind, bytes, time) or None"""
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

    def echo(self, head):
        opts = struct.unpack_from('<H', head, 3)[0]
        length = struct.unpack_from('<H', head, 1)[0]
        self.want = length
        os.write(self.fd, struct.pack('<HHH', 4, opts, length))

    def frame(self, data):
        os.write(self.fd, struct.pack('<HHH', 4 + len(data), 2, len(data)) + data)


def header(hub, timeout=1.5):
    t = hub.token(timeout)
    return t if t and t[0] == 'header' else (t or ('none', b'', 0))


def main():
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        exe = d / 'sensord'
        build = subprocess.run(['cc', '-O2', '-std=c11', '-Wall', '-Wno-unused-parameter', '-Werror',
                                '-o', str(exe), str(SOURCE), '-lm'], capture_output=True, text=True)
        check(build.returncode == 0, 'builds with -Wall, no warnings ' + build.stderr.strip())
        if build.returncode:
            return
        root = d / 'root'
        for sub in ('run/user/1000', 'var/lib'):
            (root / sub).mkdir(parents=True)
        (root / 'run/zrnperfd.status').write_text('screen=on\n')
        seq = d / 'resume_seq'
        seq.write_text('0\n')
        master, slave = pty.openpty()
        tty.setraw(slave)
        tty.setraw(master)
        env = dict(os.environ, ZTOSCSF_SSP=os.ttyname(slave), ZTOSCSF_RESUME_SEQ=str(seq),
                   ZTOSCSF_ROOT=str(root))
        err = open(d / 'stderr', 'w')
        proc = subprocess.Popen([str(exe)], env=env, stderr=err, stdout=err)
        hub = Hub(master)
        ctl = root / 'run/ztoscsf-sensord.ctl'

        def say(line):
            fd = os.open(ctl, os.O_WRONLY)
            os.write(fd, (line + '\n').encode())
            os.close(fd)

        try:
            t = hub.token(3)
            check(t is not None and t[0] == 'time', 'start: the time first, on an idle link')
            kind, light, _ = header(hub)
            check(kind == 'header' and light[0] == 0xa1, 'start: ADD light')
            hub.echo(light)
            t = hub.token(1)
            check(t and t[0] == 'payload' and t[1][0] == 9 and struct.unpack_from('<i', t[1], 1)[0] == 1000,
                  'start: ADD light payload (id 9, 1000 ms)')

            # (e) the time waits for the open transaction
            say('watch accel')
            kind, accel, _ = header(hub)
            check(kind == 'header' and accel[0] == 0xa1, '(e) watch accel: ADD header')
            hub.frame(bytes([0x06]))		# a frame: frame() asks for the time
            check(hub.token(0.3) is None, '(e) no time message while ADD accel is open')
            hub.echo(accel)
            t = hub.token(1)
            check(t and t[0] == 'payload' and t[1][0] == 0, '(e) ADD accel payload')
            t = hub.token(1)
            check(t and t[0] == 'time', '(e) then the time message')

            # (a) two REMOVEs, the second after the first one's echo and payload
            say('screen off')
            kind, rm1, _ = header(hub)
            check(kind == 'header' and rm1[0] == 0xa2, '(a) screen off: REMOVE light header')
            check(hub.token(0.3) is None, '(a) no second header before the echo')
            hub.echo(rm1)
            t = hub.token(1)
            check(t and t[0] == 'payload' and len(t[1]) == 5 and t[1][0] == 9, '(a) REMOVE light payload, 5 bytes')
            kind, rm2, _ = header(hub)
            check(kind == 'header' and rm2[0] == 0xa2, '(a) then REMOVE accel header')
            hub.echo(rm2)
            t = hub.token(1)
            check(t and t[0] == 'payload' and t[1][0] == 0, '(a) REMOVE accel payload')

            # (b) no echo: dropped after 1000 ms, the next header follows
            say('screen on')
            kind, add1, t1 = header(hub)
            check(kind == 'header' and add1[0] == 0xa1, '(b) screen on: ADD light header')
            kind, add2, t2 = header(hub, 2.5)
            check(kind == 'header' and add2[0] == 0xa1 and 0.9 <= t2 - t1 <= 1.4,
                  f'(b) unanswered: next header {t2 - t1:.3f} s later (1.0 s)')

            # (c) a resume during an open transaction: 0xd3 after its payload
            seq.write_text('1\n')
            say('')			# wakes the loop, as a sysfs notify would
            check(hub.token(0.3) is None, '(c) nothing while ADD accel is open')
            hub.echo(add2)
            t = hub.token(1)
            check(t and t[0] == 'payload' and len(t[1]) == 10, '(c) ADD accel payload first')
            kind, d3, _ = header(hub)
            check(kind == 'header' and d3 == bytes([0xd3, 0, 0, 1, 0, 0, 0, 0, 0]),
                  '(c) then exactly D3 00 00 01 00 00 00 00 00: ' + d3.hex(' '))
            check(hub.token(0.3) is None, '(c) a bare command holds nothing open')

            # (d) within 5 s of the resume an echo is waited for 400 ms
            say('screen off')
            kind, rm3, t3 = header(hub)
            check(kind == 'header' and rm3[0] == 0xa2, '(d) REMOVE light header')
            kind, rm4, t4 = header(hub, 2)
            check(kind == 'header' and rm4[0] == 0xa2 and 0.35 <= t4 - t3 <= 0.7,
                  f'(d) after a resume: next header {t4 - t3:.3f} s later (0.4 s)')
            hub.echo(rm4)
            t = hub.token(1)
            check(t and t[0] == 'payload', '(d) REMOVE accel payload')

            proc.send_signal(signal.SIGTERM)
            proc.wait(3)
            check(proc.returncode == 0, 'stops cleanly')
        finally:
            if proc.poll() is None:
                proc.kill()
            err.close()
            log = (d / 'stderr').read_text()
            print(log, end='')
            check(log.count('no echo for 0xa1') == 1 and log.count('no echo for 0xa2') == 1,
                  'two "no echo" lines, one for each unanswered header')
            check(log.count('sent 0xd3') == 1, 'one 0xd3 sent')
    print('PASS' if not failures else f'{len(failures)} FAILED')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
