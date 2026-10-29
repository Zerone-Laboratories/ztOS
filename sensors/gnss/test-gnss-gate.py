#!/usr/bin/env python3
"""Host-only GNSS lifecycle regression tests; never invoke systemctl."""
import importlib.machinery
import importlib.util
from pathlib import Path
import socket
import unittest
from unittest.mock import patch

GATE = (Path(__file__).resolve().parents[2] / 'devices/samsung-starlte/overlay'
        / 'usr/local/sbin/ztos-gnss-gate')


class StopPump(Exception):
    pass


class GateTests(unittest.TestCase):
    def setUp(self):
        loader = importlib.machinery.SourceFileLoader('gnss_gate', str(GATE))
        self.gate = importlib.util.module_from_spec(
            importlib.util.spec_from_loader(loader.name, loader))
        loader.exec_module(self.gate)
        self.reader, self.peer = socket.socketpair()
        self.reader.setblocking(False)
        self.gate.clients.add(self.reader)
        self.addCleanup(self.reader.close)
        self.addCleanup(self.peer.close)

    def test_live_reader_is_retained(self):
        self.gate.prune_readers()
        self.assertEqual(self.gate.clients, {self.reader})

    def test_input_does_not_hide_disconnect(self):
        self.peer.sendall(b'ignored client input')
        self.peer.close()
        self.gate.prune_readers()
        self.gate.prune_readers()
        self.assertFalse(self.gate.clients)

    def check_idle_stop(self, silent):
        gate = self.gate
        now = [0]
        actions = []
        gate.running = True
        gate.last_reader = 0
        self.peer.close()

        def tick(*args):
            now[0] += 1
            if now[0] > gate.IDLE + 10:
                raise StopPump()

        def unit(action):
            actions.append((action, now[0]))

        class Inner:
            def settimeout(self, timeout):
                pass

            def recv(self, size):
                tick()
                raise socket.timeout()

            def close(self):
                pass

        with patch.object(gate.time, 'monotonic', side_effect=lambda: now[0]), \
             patch.object(gate.time, 'sleep', side_effect=tick), \
             patch.object(gate.arrived, 'wait', side_effect=tick), \
             patch.object(gate, 'unit', side_effect=unit), \
             patch.object(gate.socket, 'create_connection',
                          **({'return_value': Inner()} if silent else
                             {'side_effect': ConnectionRefusedError()})):
            with self.assertRaises(StopPump):
                gate.pump()
        self.assertEqual(actions, [('stop', gate.IDLE + 1)])
        self.assertFalse(gate.running)
        self.assertFalse(gate.clients)

    def test_disconnect_stops_unavailable_engine(self):
        self.check_idle_stop(silent=False)

    def test_disconnect_stops_silent_engine(self):
        self.check_idle_stop(silent=True)

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

    def test_void_rmc_and_bad_checksum_ignored(self):
        gate = self.gate
        gate.last_fix = None
        gate.track(self.sentence('GPRMC,120000.00,V,,,,,,,291026,,,N'))
        gate.track('$GPRMC,120000.00,A,1030.0000,S,02015.0000,E,0.0,0.0,291026,,,A*00')
        self.assertIsNone(gate.last_fix)
        self.assertIsNone(gate.last_sentence())


if __name__ == '__main__':
    unittest.main()
