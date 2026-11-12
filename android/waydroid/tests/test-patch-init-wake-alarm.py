#!/usr/bin/env python3
"""patch-init-wake-alarm.py on a copy of the image's init.

	test-patch-init-wake-alarm.py [INIT]

INIT defaults to $ZTOS_WAYDROID_INIT; the test is skipped without one.
"""
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent / 'patch-init-wake-alarm.py'
REPO = HERE.parents[2]
OBJDUMP = REPO / 'build/toolchain/arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-objdump'
INIT = os.environ.get('ZTOS_WAYDROID_INIT', '')
if __name__ == '__main__' and len(sys.argv) > 1 and not sys.argv[1].startswith('-'):
    INIT = sys.argv.pop(1)
PATCHED = 'd01768fd0cf0b5a18557a9d721f3b6b2df42392905a892abf84c8c77debf1e08'


def run(path):
    return subprocess.run([sys.executable, str(SCRIPT), str(path)], capture_output=True, text=True)


@unittest.skipUnless(INIT and Path(INIT).is_file(), 'no init given')
class PatchInitTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.copy = Path(self.dir.name) / 'init'
        shutil.copyfile(INIT, self.copy)
        os.chmod(self.copy, 0o750)

    def tearDown(self):
        self.dir.cleanup()

    def test_patch_changes_exactly_four_bytes(self):
        result = run(self.copy)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(PATCHED, result.stdout)
        cmp = subprocess.run(['cmp', '-l', INIT, str(self.copy)], capture_output=True, text=True)
        changed = cmp.stdout.split('\n')
        changed = [line for line in changed if line.strip()]
        # cmp -l counts from 1: bytes 0xf4b88..0xf4b8b
        self.assertEqual([int(line.split()[0]) - 1 for line in changed],
                         [0xF4B88, 0xF4B89, 0xF4B8A, 0xF4B8B])
        self.assertEqual(stat.S_IMODE(self.copy.stat().st_mode), 0o750)
        if OBJDUMP.exists():
            dis = subprocess.run([str(OBJDUMP), '-d', '--start-address=0xf4b80',
                                  '--stop-address=0xf4b90', str(self.copy)],
                                 capture_output=True, text=True).stdout
            line = next(l for l in dis.splitlines() if l.strip().startswith('f4b88:'))
            self.assertIn('nop', line)
            self.assertIn('mov\tw0, #0x23', dis)

    def test_second_run_is_a_no_op(self):
        self.assertEqual(run(self.copy).returncode, 0)
        before = self.copy.read_bytes()
        result = run(self.copy)
        self.assertEqual(result.returncode, 0)
        self.assertIn('already patched', result.stdout)
        self.assertEqual(self.copy.read_bytes(), before)

    def test_any_other_init_is_refused_untouched(self):
        data = bytearray(self.copy.read_bytes())
        data[0x1000] ^= 0xFF
        self.copy.write_bytes(bytes(data))
        result = run(self.copy)
        self.assertEqual(result.returncode, 3)
        self.assertIn('Unsupported init; no changes made.', result.stderr)
        self.assertEqual(self.copy.read_bytes(), bytes(data))


if __name__ == '__main__':
    unittest.main()
