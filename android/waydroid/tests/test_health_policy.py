"""Messaging exemptions avoid Android polling without breaking idle freeze."""
import ast
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import MagicMock


class HealthPolicyTests(unittest.TestCase):
    def policy(self, keep):
        source = ast.parse((Path(__file__).resolve().parents[1] / 'network.py').read_text())
        function = next(node for node in source.body
                        if isinstance(node, ast.FunctionDef) and node.name == 'maintain')
        calls = []

        def run(args, **kwargs):
            calls.append(args)
            output = ''
            if args[0] == 'lxc-info':
                output = 'RUNNING' if args[-1] == '-sH' else '101'
            elif args[-2:] == ['getprop', 'waydroid.open_windows']:
                output = '0'
            elif args[-1] == 'waydroid.open_windows':
                output = '0'
            elif args[-1] == 'persist.waydroid.suspend':
                output = 'true'
            return SimpleNamespace(returncode=0, stdout=output, stderr='')

        namespace = dict(pathlib=MagicMock(), run=run, LXC=[], configured={'101'},
                         idle={'101': 0}, keep_awake_active=lambda: keep,
                         launch_in_progress=lambda: False,
                         time=SimpleNamespace(monotonic=lambda: 20))
        exec(compile(ast.Module(body=[function], type_ignores=[]), '<maintain>', 'exec'), namespace)
        namespace['maintain']()
        return calls, namespace['idle']

    def test_messaging_exemption_does_not_attach_or_freeze(self):
        calls, idle = self.policy(True)
        self.assertFalse(any(call[0] in ('lxc-attach', 'waydroid') for call in calls))
        self.assertEqual(idle, {})

    def test_no_exemption_still_freezes_idle_container(self):
        calls, idle = self.policy(False)
        self.assertIn(['waydroid', 'container', 'freeze'], calls)
        self.assertEqual(idle, {})


if __name__ == '__main__':
    unittest.main()
