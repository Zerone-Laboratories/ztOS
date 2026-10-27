#!/usr/bin/env python3
"""Exercise sleep inhibition and directory watches without phone hardware."""
import ast
import importlib.machinery
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


SOURCE = Path(__file__).resolve().parents[3] / 'rootfs/overlay/usr/local/sbin/zrn-sleepd'
# its ztos_user, from the tree rather than /usr/lib/ztos/python
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'os/overlay/usr/lib/ztos/python'))
loader = importlib.machinery.SourceFileLoader('background_sleepd', str(SOURCE))
spec = importlib.util.spec_from_loader(loader.name, loader)
sleepd = importlib.util.module_from_spec(spec)
loader.exec_module(sleepd)


class BackgroundSleepTest(unittest.TestCase):
    def test_events_create_replace_remove_and_legacy(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            cache = sleepd.BackgroundApps(home)
            try:
                self.assertFalse(cache.held())
                config = home / '.config/ztos'
                config.mkdir(parents=True)
                apps = config / 'background-apps'
                apps.write_text('# comment\n\nfirefox\n')
                self.assertTrue(cache.held())
                replacement = config / 'replacement'
                replacement.write_text('# no apps\n')
                replacement.replace(apps)
                self.assertFalse(cache.held())
                legacy = config / 'android.conf'
                legacy.write_text('keep_awake=, com.whatsapp ,\n')
                self.assertTrue(cache.held())
                legacy.write_text('keep_awake= , , # empty\n')
                self.assertFalse(cache.held())
                apps.write_text('waydroid.com.whatsapp\n')
                self.assertTrue(cache.held())
                apps.unlink()
                self.assertFalse(cache.held())
                legacy.unlink()
                config.rmdir()
                self.assertFalse(cache.held())
                config.mkdir()
                apps.write_text('ztos-android-com.whatsapp\n')
                self.assertTrue(cache.held())
            finally:
                cache.close()

    def test_cached_checks_do_not_read_config(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = sleepd.BackgroundApps(directory)
            try:
                cache.held()
                with patch.object(Path, 'read_text', side_effect=AssertionError('polled config')):
                    self.assertFalse(cache.held())
                    self.assertFalse(cache.held())
            finally:
                cache.close()

    def test_any_background_entry_prevents_cycle_preparation(self):
        with patch.object(sleepd, 'background_apps_active', return_value=True), \
                patch.object(sleepd, 'run') as run, \
                patch.object(sleepd, 'write') as write:
            self.assertEqual(sleepd.cycle(), ('background apps', 0))
            run.assert_not_called()
            write.assert_not_called()

    def test_daemon_reports_background_inhibitor(self):
        with patch.object(sleepd, 'ENABLED') as enabled, \
                patch.object(sleepd, 'screen_off', return_value=True), \
                patch.object(sleepd, 'background_apps_active', return_value=True):
            enabled.exists.return_value = True
            self.assertEqual(sleepd.why_awake(), 'background apps')

    def test_once_cannot_override_background_inhibitor(self):
        command = ast.Module(body=[ast.parse(SOURCE.read_text()).body[-1]], type_ignores=[])
        with patch.dict(sleepd.__dict__, {'__name__': '__main__'}), \
                patch.object(sleepd.sys, 'argv', ['zrn-sleepd', 'once']), \
                patch.object(sleepd, 'background_apps_active', return_value=True), \
                patch.object(sleepd, 'setup') as setup, patch.object(sleepd, 'log'):
            with self.assertRaises(SystemExit) as exit_status:
                exec(compile(command, str(SOURCE), 'exec'), sleepd.__dict__)
            self.assertEqual(exit_status.exception.code, 1)
            setup.assert_not_called()


if __name__ == '__main__':
    unittest.main()
