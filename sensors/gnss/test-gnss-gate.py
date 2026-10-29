#!/usr/bin/env python3
"""Host-only GNSS lifecycle regression tests; never invoke systemctl."""
import importlib.machinery
import importlib.util
import os
from pathlib import Path
import socket
import tempfile
import time
import unittest
from unittest.mock import patch

GATE = (Path(__file__).resolve().parents[2] / 'devices/samsung-starlte/overlay'
        / 'usr/local/sbin/ztos-gnss-gate')


class StopPump(Exception):
    pass


def load_gate():
    loader = importlib.machinery.SourceFileLoader('gnss_gate', str(GATE))
    gate = importlib.util.module_from_spec(
        importlib.util.spec_from_loader(loader.name, loader))
    loader.exec_module(gate)
    return gate


class GateBase(unittest.TestCase):
    def setUp(self):
        self.gate = gate = load_gate()
        self.reader, self.peer = socket.socketpair()
        self.reader.setblocking(False)
        gate.clients.add(self.reader)
        self.addCleanup(self.reader.close)
        self.addCleanup(self.peer.close)
        # a private /run: zrn-sleepd's inhibit dir, keep flags, the hub
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        gate.SLEEPD = str(self.tmp / 'zrn-sleepd')
        gate.INHIBIT_DIR = gate.SLEEPD + '/inhibit'
        gate.INHIBIT = gate.INHIBIT_DIR + '/timed-gnss'
        gate.INHIBIT_TMP = gate.SLEEPD + '/.gnss.tmp'
        gate.KEEP_GLOB = str(self.tmp / 'keep-*')
        gate.KEEP_SCREEN_GLOB = str(self.tmp / 'user-*' / 'keep-screen')
        gate.LAST = str(self.tmp / 'last')
        self.inhibit = Path(gate.INHIBIT)
        # boot clock and screen under test control; no systemctl, no threads
        self.clock = [1000.0]
        self.screen = [False]   # screen_off()
        self.ready = [True]
        self.actions = []
        self.runs = []
        for p in (
                patch.object(gate, 'now', side_effect=lambda: self.clock[0]),
                patch.object(gate, 'screen_off', side_effect=lambda: self.screen[0]),
                patch.object(gate, 'hub_ready', side_effect=lambda: self.ready[0]),
                patch.object(gate, 'unit', side_effect=self.unit),
                patch.object(gate, 'spawn', side_effect=lambda fn, *a: fn(*a)),
                patch.object(gate.subprocess, 'run',
                             side_effect=lambda cmd, **kw: self.runs.append((cmd[1], self.clock[0])))):
            p.start()
            self.addCleanup(p.stop)
        gate.last_reader = self.clock[0]
        gate.ENGINE_SEEN = 1e9   # the engine answers, unless a test says not
        gate.engine_seen = 0

    def unit(self, action, wait=False):
        self.actions.append((action, self.clock[0]))

    def tick(self, s=1.0):
        self.clock[0] += s

    def expiry(self):
        return self.inhibit.stat().st_mtime - time.time()


class LifecycleTests(GateBase):
    def test_live_reader_is_retained(self):
        self.gate.prune_readers()
        self.assertEqual(self.gate.clients, {self.reader})

    def test_input_does_not_hide_disconnect(self):
        self.peer.sendall(b'ignored client input')
        self.peer.close()
        self.gate.prune_readers()
        self.gate.prune_readers()
        self.assertFalse(self.gate.clients)

    def run_pump(self, silent, limit):
        gate = self.gate

        def tick(*args, **kw):
            self.tick()
            if self.clock[0] > 1000 + limit:
                raise StopPump()

        class Inner:
            def settimeout(self, timeout):
                pass

            def recv(self, size):
                tick()
                raise socket.timeout()

            def close(self):
                pass

        with patch.object(gate.time, 'sleep', side_effect=tick), \
             patch.object(gate.arrived, 'wait', side_effect=tick), \
             patch.object(gate.socket, 'create_connection',
                          **({'return_value': Inner()} if silent else
                             {'side_effect': ConnectionRefusedError()})):
            with self.assertRaises(StopPump):
                gate.pump()

    def check_idle_stop(self, silent, off):
        gate = self.gate
        self.screen[0] = off
        idle = gate.IDLE_OFF if off else gate.IDLE
        gate.running = True
        self.peer.close()
        self.run_pump(silent, idle + 10)
        stops = [a for a in self.actions if a[0] == 'stop']
        self.assertEqual(stops, [('stop', 1000 + idle + 1)])
        self.assertFalse(gate.running)
        self.assertFalse(gate.clients)
        self.assertFalse(self.inhibit.exists())

    def test_disconnect_stops_unavailable_engine(self):
        self.check_idle_stop(silent=False, off=False)

    def test_disconnect_stops_silent_engine(self):
        self.check_idle_stop(silent=True, off=False)

    def test_screen_off_idle_is_short(self):
        self.assertLess(self.gate.IDLE_OFF, 15)  # zrn-sleepd's SCREEN_OFF_S
        self.check_idle_stop(silent=True, off=True)

    def test_screen_going_off_ends_the_idle_hold(self):
        gate = self.gate
        gate.running = True
        self.gate.clients.clear()
        gate.last_reader = 1000
        self.clock[0] = 1060   # 60 s into the 120 s hold, screen on
        gate.policy()
        self.assertTrue(gate.running)
        self.screen[0] = True
        gate.policy()
        self.assertFalse(gate.running)
        self.assertEqual(self.actions, [('stop', 1060)])

    def test_retry_resets_start_limit_only_for_a_steady_hub(self):
        gate = self.gate
        gate.running = True
        gate.last_start = 0
        self.ready[0] = False
        self.run_pump(silent=False, limit=40)
        self.assertNotIn('reset-failed', [r[0] for r in self.runs])
        self.assertIn('start', [a[0] for a in self.actions])
        # the hub is back: 10 s ready before the start limit is reset
        self.ready[0] = True
        self.actions.clear()
        gate.last_start = self.clock[0]
        start = self.clock[0]
        self.run_pump(silent=False, limit=start - 1000 + 40)
        resets = [t for cmd, t in self.runs if cmd == 'reset-failed']
        self.assertTrue(resets)
        self.assertGreaterEqual(resets[0] - start, gate.HUB_READY_HELD)


class InhibitTests(GateBase):
    def test_atomic_write_and_expiry(self):
        gate = self.gate
        seen = []
        real_replace = os.replace

        def replace(src, dst):
            # the file is already dated before it appears in inhibit/
            seen.append((src, dst, os.stat(src).st_mtime - time.time()))
            real_replace(src, dst)

        with patch.object(gate.os, 'replace', side_effect=replace):
            with gate.lock:
                gate.hold()
        src, dst, ahead = seen[0]
        self.assertEqual(os.path.dirname(src), gate.SLEEPD)  # outside inhibit/
        self.assertEqual(dst, gate.INHIBIT)
        self.assertAlmostEqual(ahead, gate.HOLD_FOR, delta=2)
        self.assertAlmostEqual(self.expiry(), gate.HOLD_FOR, delta=2)
        self.assertFalse(os.path.exists(gate.INHIBIT_TMP))

    def test_refresh_while_serving(self):
        gate = self.gate
        self.screen[0] = True
        gate.running = True
        gate.policy()
        first = gate.held_at
        self.assertTrue(self.inhibit.exists())
        self.tick(gate.HOLD_EVERY - 1)
        gate.policy()
        self.assertEqual(gate.held_at, first)          # not yet
        self.tick(1)
        gate.policy()
        self.assertEqual(gate.held_at, first + gate.HOLD_EVERY)
        # removed under it (the unit's ExecStopPost): written again at once
        self.inhibit.unlink()
        self.tick(1)
        gate.policy()
        self.assertTrue(self.inhibit.exists())

    def test_last_reader_screen_on_releases(self):
        gate = self.gate
        gate.running = True
        gate.policy()
        self.assertTrue(self.inhibit.exists())
        gate.clients.clear()
        self.tick()
        gate.policy()
        self.assertTrue(gate.running)                  # idle hold, screen on
        self.assertFalse(self.inhibit.exists())

    def test_last_reader_screen_off_holds_until_stopped(self):
        gate = self.gate
        self.screen[0] = True
        gate.running = True
        gate.policy()
        gate.clients.clear()
        self.tick()
        gate.policy()
        self.assertTrue(self.inhibit.exists())         # engine still on
        stopped = []
        gate.spawn.side_effect = lambda fn, *a: stopped.append((fn, a))
        self.tick(gate.IDLE_OFF)
        gate.policy()
        self.assertFalse(gate.running)
        self.assertTrue(self.inhibit.exists())         # still stopping
        self.assertAlmostEqual(self.expiry(), gate.STOP_HOLD, delta=2)
        fn, a = stopped[0]
        fn(*a)                                         # systemctl stop returned
        self.assertFalse(self.inhibit.exists())

    def test_no_hold_for_a_dead_engine(self):
        gate = self.gate
        gate.ENGINE_SEEN = 30
        gate.engine_seen = -1e9
        self.screen[0] = True
        self.ready[0] = False
        with gate.lock:
            gate.start_engine('reader arrived; GPS on')
        self.assertTrue(self.inhibit.exists())          # the start is covered
        self.tick(gate.START_HOLD - 1)
        gate.policy()
        self.assertTrue(self.inhibit.exists())
        self.tick(2)
        gate.policy()
        self.assertFalse(self.inhibit.exists())         # never answered: let go
        self.assertTrue(gate.running)                   # the reader still waits
        gate.engine_seen = self.clock[0]                 # the engine came up
        gate.policy()
        self.assertTrue(self.inhibit.exists())

    def test_no_hold_without_readers(self):
        gate = self.gate
        gate.clients.clear()
        gate.running = True
        gate.policy()
        self.assertFalse(self.inhibit.exists())

    def test_cap(self):
        gate = self.gate
        self.screen[0] = True
        gate.running = True
        gate.policy()
        self.tick(gate.CAP - 1)
        gate.policy()
        self.assertTrue(gate.running)
        self.tick(1)
        with patch.object(gate, 'log') as log:
            gate.policy()
        self.assertIn('screen-off location cap', log.call_args_list[0][0][0])
        self.assertFalse(gate.running)
        self.assertTrue(gate.capped)
        self.assertFalse(self.inhibit.exists())
        self.assertEqual([a[0] for a in self.actions], ['stop'])
        # a new reader with the screen still off: refused
        with gate.lock:
            gate.start_engine('reader arrived; GPS on')
        self.assertFalse(gate.running)
        self.assertFalse(self.inhibit.exists())
        # screen on: the waiting reader gets its GPS back
        self.screen[0] = False
        self.tick()
        gate.policy()
        self.assertTrue(gate.running)
        self.assertFalse(gate.capped)
        self.assertEqual(self.actions[-1][0], 'start')

    def test_cap_counts_from_serving_not_from_screen_off(self):
        gate = self.gate
        self.screen[0] = True
        gate.clients.clear()
        gate.policy()                       # screen off, nobody reading
        self.tick(gate.CAP * 2)
        gate.clients.add(self.reader)
        gate.running = True
        gate.policy()
        self.assertTrue(gate.running)

    def test_keep_flag_bypasses_cap(self):
        gate = self.gate
        (self.tmp / 'keep-navigation').touch()
        self.screen[0] = True
        gate.running = True
        gate.policy()
        self.tick(gate.CAP + 5)
        gate.policy()
        self.assertTrue(gate.running)
        self.assertTrue(self.inhibit.exists())

    def test_fresh_keep_screen_bypasses_cap_stale_does_not(self):
        gate = self.gate
        d = self.tmp / 'user-1000'
        d.mkdir()
        f = d / 'keep-screen'
        f.touch()
        self.assertTrue(gate.kept())
        old = time.time() - gate.KEEP_SCREEN_FRESH - 1
        os.utime(f, (old, old))
        self.assertFalse(gate.kept())


class NmeaTests(GateBase):
    def sentence(self, body):
        return '$%s*%s' % (body, self.gate.nmea_checksum(body))

    def test_last_fix_from_nmea(self):
        gate = self.gate
        gate.last_fix = None
        with patch.object(gate, 'save_last'):
            # made-up position, not anyone's
            gate.track(self.sentence('GPGGA,120000.00,1030.0000,S,02015.0000,E,1,07,1.5,10.0,M,,M,,'))
            gate.track(self.sentence('GPRMC,120000.00,A,1030.0000,S,02015.0000,E,0.0,0.0,291026,,,A'))
        lat, lon, acc, _ = gate.last_fix
        self.assertAlmostEqual(lat, -10.5)
        self.assertAlmostEqual(lon, 20.25)
        self.assertEqual(acc, 6.0)
        hello = gate.last_sentence().decode().strip()
        body, _, cs = hello[1:].partition('*')
        self.assertTrue(body.startswith('PZTLF,-10.500000,20.250000,6,'))
        self.assertEqual(cs, gate.nmea_checksum(body))

    def test_engine_accuracy_preferred(self):
        gate = self.gate
        rmc = self.sentence('GPRMC,120000.00,A,1030.0000,S,02015.0000,E,0.0,0.0,291026,,,A')
        gga = self.sentence('GPGGA,120000.00,1030.0000,S,02015.0000,E,1,07,1.5,10.0,M,,M,,')
        with patch.object(gate, 'save_last'):
            gate.track(gga)
            gate.track(self.sentence('PZTFX,3.2,,'))      # before its RMC
            gate.track(rmc)
            self.assertAlmostEqual(gate.last_fix[2], 3.2)
            self.tick(5)                                  # stale: HDOP again
            gate.track(gga)
            gate.track(rmc)
            self.assertEqual(gate.last_fix[2], 6.0)
            gate.track(self.sentence('PZTFX,11.5,,'))     # after its RMC
            self.assertAlmostEqual(gate.last_fix[2], 11.5)
            gate.track(self.sentence('PZTFX,-1,,'))       # nonsense: ignored
            self.assertAlmostEqual(gate.last_fix[2], 11.5)

    def test_void_rmc_and_bad_checksum_ignored(self):
        gate = self.gate
        gate.last_fix = None
        gate.track(self.sentence('GPRMC,120000.00,V,,,,,,,291026,,,N'))
        gate.track('$GPRMC,120000.00,A,1030.0000,S,02015.0000,E,0.0,0.0,291026,,,A*00')
        self.assertIsNone(gate.last_fix)
        self.assertIsNone(gate.last_sentence())

    def test_slow_reader_gets_whole_sentences(self):
        gate = self.gate
        self.reader.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4096)
        lines = [self.sentence('GPGSV,3,1,12,%02d,40,083,46,02,17,308,41' % i)
                 for i in range(2000)]
        sent = 0
        got = b''
        with gate.lock:
            for i in range(0, len(lines), 10):
                gate.feed(self.reader, ''.join(l + '\r\n' for l in lines[i:i + 10]).encode())
                sent += 1
                if i % 200 == 0:     # the reader catches up now and then
                    self.peer.setblocking(False)
                    try:
                        while True:
                            got += self.peer.recv(65536)
                    except BlockingIOError:
                        pass
            self.assertIn(self.reader, gate.clients)
            self.peer.setblocking(False)
            while gate.backlog.get(self.reader):
                try:
                    while True:
                        got += self.peer.recv(65536)
                except BlockingIOError:
                    pass
                gate.feed(self.reader, b'')
            try:
                while True:
                    got += self.peer.recv(65536)
            except BlockingIOError:
                pass
        out = got.decode().split('\r\n')
        self.assertEqual(out[-1], '')
        self.assertEqual(out[:-1], lines)   # all of them, none spliced

    def test_stuck_reader_is_dropped(self):
        gate = self.gate
        chunk = (self.sentence('GPGSV,3,1,12,01,40,083,46') + '\r\n').encode() * 200
        with gate.lock:
            for _ in range(200):
                if self.reader not in gate.clients:
                    break
                gate.feed(self.reader, chunk)
        self.assertNotIn(self.reader, gate.clients)
        self.assertNotIn(self.reader, gate.backlog)


if __name__ == '__main__':
    unittest.main()
