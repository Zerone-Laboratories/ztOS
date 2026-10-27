"""Exercise compositor-generation recovery with bounded fake runtime commands."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

LAUNCH = Path(__file__).resolve().parents[1] / 'launch.sh'


class CompositorGenerationTests(unittest.TestCase):
    def trial(self, previous):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            commands = root / 'commands'
            commands.mkdir()
            events = root / 'events'
            (root / 'wayland-0').touch()
            generation = subprocess.check_output(
                ['stat', '-Lc', '%d:%i:%z', str(root / 'wayland-0')], text=True).strip()
            state = root / 'ztos-waydroid-compositor'
            if previous is not None:
                state.write_text(generation if previous == 'same' else previous)
            bodies = {
                'systemctl': 'exit 0',
                'ztos-waydroid-power': '[ ! -e "$XDG_RUNTIME_DIR/ztos-waydroid-launching" ] || exit 7; '
                                       'printf "power %s\\n" "$*" >> "$EVENTS"',
                'busctl': 'exit 0',
                'sudo': 'printf "sudo %s\\n" "$*" >> "$EVENTS"',
                'waydroid': 'printf "waydroid %s\\n" "$*" >> "$EVENTS"; '
                            '[ "$1" != prop ] || echo 1',
            }
            for name, body in bodies.items():
                path = commands / name
                path.write_text('#!/bin/bash\n' + body + '\n')
                path.chmod(0o755)
            env = dict(os.environ, PATH=str(commands) + ':' + os.environ['PATH'],
                       XDG_RUNTIME_DIR=str(root), WAYLAND_DISPLAY='wayland-0', EVENTS=str(events))
            result = subprocess.run(['bash', str(LAUNCH), 'com.whatsapp'], env=env,
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(state.read_text().strip(), generation)
            self.assertFalse((root / 'ztos-waydroid-launching').exists())
            self.assertTrue(events.read_text().endswith('power sync\n'))
            return events.read_text()

    def test_first_session_records_compositor_without_recovery(self):
        self.assertNotIn('recover recover', self.trial(None))

    def test_same_compositor_keeps_session(self):
        self.assertNotIn('recover recover', self.trial('same'))

    def test_changed_compositor_recovers_before_first_app_launch(self):
        events = self.trial('old-socket')
        self.assertEqual(events.count('recover recover'), 1)
        self.assertLess(events.index('recover recover'), events.index('app launch'))


if __name__ == '__main__':
    unittest.main()
