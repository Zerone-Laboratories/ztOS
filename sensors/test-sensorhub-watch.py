#!/usr/bin/env python3
"""Exercise only the user-space supervisor, with no phone or hardware access."""
import pathlib
import subprocess
import tempfile
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "rootfs/overlay/usr/local/sbin/ztoscsf-sensorhub"


class SensorhubWatch(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = pathlib.Path(self.temp.name)
        self.ready = self.path / "ready"
        self.pid = self.path / "pid"
        child = self.path / "lhd"
        child.write_text(f'#!/bin/sh\necho $$ > "{self.pid}"\nexec /bin/sleep 60\n')
        child.chmod(0o755)
        source = SOURCE.read_text().replace("/usr/bin/chroot", str(child))
        source = source.replace("/sys/class/sec/sensorhub/ready", str(self.ready))
        source = source.replace("sleep 1", "sleep 0.01").replace("sleep 2", "sleep 0.01")
        self.script = self.path / "watch"
        self.script.write_text(source)

    def start(self):
        process = subprocess.Popen(
            ["sh", str(self.script), "--run"], stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True)
        self.addCleanup(lambda: process.poll() is None and process.kill())
        return process

    def assert_child_stopped(self):
        pid = int(self.pid.read_text())
        status = pathlib.Path(f"/proc/{pid}/stat")
        for _ in range(100):
            if not status.exists() or status.read_text().split()[2] == "Z":
                return
            time.sleep(0.01)
        self.fail(f"mock lhd still running: {pid}")

    def test_never_ready(self):
        self.ready.write_text("0\n")
        process = self.start()
        output = process.communicate(timeout=3)[0]
        self.assertEqual(process.returncode, 1)
        self.assertIn("firmware not ready after 30 seconds", output)
        self.assert_child_stopped()

    def test_missing_ready(self):
        process = self.start()
        self.assertIn("firmware not ready", process.communicate(timeout=3)[0])
        self.assertEqual(process.returncode, 1)
        self.assert_child_stopped()

    def test_ready_then_crash(self):
        self.ready.write_text("1\n")
        process = self.start()
        time.sleep(0.1)
        self.assertIsNone(process.poll())
        self.ready.write_text("0\n")
        self.assertIn("firmware lost readiness", process.communicate(timeout=3)[0])
        self.assertEqual(process.returncode, 1)
        self.assert_child_stopped()

    def test_stop_ready_hub(self):
        self.ready.write_text("1\n")
        process = self.start()
        time.sleep(0.1)
        process.terminate()
        process.communicate(timeout=3)
        self.assertEqual(process.returncode, 0)
        self.assert_child_stopped()


if __name__ == "__main__":
    unittest.main()
