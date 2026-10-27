"""Power workers obey current focus, including a focus change during thaw."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PowerOrderingTests(unittest.TestCase):
    def trial(self, state, change=False, marker=False):
        with tempfile.TemporaryDirectory(dir=ROOT / 'results') as directory:
            runtime = Path(directory)
            policy = runtime / 'ztos-waydroid-power-state'
            policy.write_text(state + '\n')
            commands = runtime / 'commands'
            commands.mkdir()
            sudo = commands / 'sudo'
            sudo.write_text('''#!/bin/bash
printf '%s\\n' "$3" >> "$XDG_RUNTIME_DIR/events"
if [[ $3 == unfreeze && $CHANGE_POLICY == 1 ]]; then
    printf 'idle\\n' > "$XDG_RUNTIME_DIR/ztos-waydroid-power-state"
fi
''')
            sudo.chmod(0o755)
            launch = runtime / 'ztos-waydroid-launching'
            if marker:
                launch.write_text(str(os.getpid()))
            env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime),
                       PATH=str(commands) + ':' + os.environ['PATH'],
                       CHANGE_POLICY='1' if change else '0')
            subprocess.run(['bash', str(ROOT / 'power.sh'), 'sync'], env=env, check=True, timeout=5)
            events = runtime / 'events'
            first = events.read_text().splitlines() if events.exists() else []
            if marker:
                launch.unlink()
                subprocess.run(['bash', str(ROOT / 'power.sh'), 'sync'], env=env, check=True, timeout=5)
            return first, events.read_text().splitlines() if events.exists() else []

    def test_stale_worker_uses_latest_idle_state(self):
        self.assertEqual(self.trial('idle')[0], ['freeze'])

    def test_home_during_thaw_finishes_frozen(self):
        self.assertEqual(self.trial('active', change=True)[0], ['unfreeze', 'freeze'])

    def test_marker_cleanup_reconciles_deferred_idle(self):
        before, after = self.trial('idle', marker=True)
        self.assertEqual(before, [])
        self.assertEqual(after, ['freeze'])


if __name__ == '__main__':
    unittest.main()
