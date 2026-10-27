"""Runtime identity regressions for the shell matcher and idle policy."""
import ast
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[3]


class DualIdentityTests(unittest.TestCase):
    def test_shell_package_and_runtime_match(self):
        source = (ROOT/'ztmetro/ztmetro.c').read_text()
        start = source.index('static bool android_command_matches(')
        end = source.index('\nstatic void android_window_command', start)
        program = '#include <stdbool.h>\n#include <string.h>\n#include <assert.h>\n'
        program += source[start:end]
        program += '''
int main(void) {
    assert(android_command_matches("ztos-android launch com.whatsapp --runtime atl", "com.whatsapp", "atl"));
    assert(!android_command_matches("ztos-android launch com.whatsapp --runtime atl", "com.whatsapp", "waydroid"));
    assert(android_command_matches("ztos-android launch com.whatsapp --runtime waydroid", "com.whatsapp", "waydroid"));
    assert(!android_command_matches("ztos-android launch com.whatsapp --runtime waydroid", "com.whatsapp", "atl"));
    assert(!android_command_matches("ztos-android launch com.whatsapp.business --runtime waydroid", "com.whatsapp", "waydroid"));
    assert(!android_command_matches("other launch com.whatsapp --runtime waydroid", "com.whatsapp", "waydroid"));
    assert(android_command_matches("/usr/local/bin/ztos-android launch com.whatsapp --runtime waydroid", "com.whatsapp", "waydroid"));
    assert(android_command_matches("ztos-android launch com.instagram.android", "com.instagram.android", "waydroid"));
}
'''
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)/'identity.c'
            path.write_text(program)
            subprocess.run(['cc', '-Wall', '-Werror', str(path), '-o', str(path.with_suffix(''))], check=True)
            subprocess.run([str(path.with_suffix(''))], check=True)

    def test_keep_awake_reads_old_and_dual_registries(self):
        import pathlib
        source = ast.parse((ROOT/'android/waydroid/network.py').read_text())
        function = next(node for node in source.body if isinstance(node, ast.FunctionDef) and node.name == 'keep_awake_active')
        namespace = {'pathlib': pathlib, 'json': json}
        exec(compile(ast.Module(body=[function], type_ignores=[]), '<keep_awake_active>', 'exec'), namespace)
        for registry, expected in (
            ({'com.whatsapp': {'runtime': 'waydroid'}}, True),
            ({'com.whatsapp': {'runtime': 'atl'}}, False),
            ({'com.whatsapp@atl': {'runtime': 'atl'}, 'com.whatsapp@waydroid': {'runtime': 'waydroid'}}, True),
            ({'com.whatsapp@atl': {'runtime': 'atl'}}, False),
        ):
            with self.subTest(registry=registry), patch('pathlib.Path.read_text', side_effect=[
                    'keep_awake=com.whatsapp\n', json.dumps(registry)]):
                self.assertEqual(namespace['keep_awake_active'](), expected)


if __name__ == '__main__':
    unittest.main()
