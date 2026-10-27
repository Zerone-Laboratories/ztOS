#!/usr/bin/env python3
"""Exercise suspend rollback without touching hardware or system services."""
import importlib.machinery
import importlib.util
from pathlib import Path
import sys
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[2] / 'rootfs/overlay/usr/local/sbin/zrn-sleepd'
# its ztos_user, from the tree rather than /usr/lib/ztos/python
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'os/overlay/usr/lib/ztos/python'))
loader = importlib.machinery.SourceFileLoader('sleepd', str(SOURCE))
spec = importlib.util.spec_from_loader(loader.name, loader)
sleepd = importlib.util.module_from_spec(spec)
loader.exec_module(sleepd)


class RecoveryTest(unittest.TestCase):
    def exercise(self, failure=None):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            wifi = base / 'wifi'
            wifi.mkdir()
            bound = wifi / sleepd.WIFI_DEV
            bound.touch()
            enabled = base / 'enabled'
            enabled.touch()
            mark = base / 'sleeping'
            writes, commands = [], []
            entered = False
            parameters = base / 'parameters'
            ufs = base / 'ufs'

            def write(path, value):
                nonlocal entered
                path = str(path)
                writes.append((path, value))
                if path == '/sys/power/state':
                    entered = True
                if failure == path and entered:
                    raise OSError('injected recovery failure')
                if path == str(wifi / 'unbind'):
                    bound.unlink()
                if path == str(wifi / 'bind'):
                    bound.touch()

            def run(*args, **kwargs):
                commands.append(args)
                failed = False
                return SimpleNamespace(returncode=int(failed))

            with patch.multiple(sleepd, SLEEP=parameters, WIFI=wifi, UFS=ufs,
                                MARK=mark, ENABLED=enabled), \
                    patch.object(sleepd, 'read', side_effect=lambda path, default='':
                                 ('2' if path == ufs else ('N' if failure == 'audio' and entered else 'Y')
                                  if str(path).endswith('/pm_ready') else '0')), \
                    patch.object(sleepd, 'write', side_effect=write), \
                    patch.object(sleepd, 'run', side_effect=run), \
                    patch.object(sleepd, 'plugged', return_value=False), \
                    patch.object(sleepd, 'background_apps_active', return_value=False), \
                    patch.object(sleepd, 'bt_sleep_mode'), \
                    patch.object(sleepd, 'wake_reason', return_value=('rtc', 0x80000002)), \
                    patch.object(sleepd, 'tell_shell'), patch.object(sleepd, 'log'), \
                    patch.object(sleepd.ztos_user, 'USER', 'test'), \
                    patch.object(sleepd.time, 'sleep'), patch.object(sleepd.os, 'sync'):
                reason, _ = sleepd.cycle()

            self.assertIn((str(wifi / 'bind'), sleepd.WIFI_DEV), writes)
            self.assertFalse(any(command[0] in ('modprobe', 'rmmod') for command in commands))
            self.assertFalse(any('pipewire.service' in command for command in commands))
            self.assertEqual(reason, 'failed' if failure else 'rtc')
            self.assertEqual(enabled.exists(), not bool(failure))
            self.assertEqual(mark.exists(), bool(failure))

    def test_success_clears_crash_marker(self):
        self.exercise()

    def test_sysfs_failure_still_recovers_wifi_and_audio(self):
        self.exercise('/sys/power/mem_sleep')

    def test_alarm_failure_still_recovers_wifi_and_audio(self):
        self.exercise('/sys/class/rtc/rtc0/wakealarm')

    def test_audio_failure_disables_further_sleep(self):
        self.exercise('audio')


if __name__ == '__main__':
    unittest.main()
