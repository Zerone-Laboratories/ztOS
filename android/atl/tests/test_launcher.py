import json
import os
from pathlib import Path
import runpy
import struct
import subprocess
import time
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[3]
API = runpy.run_path(str(ROOT/'os/overlay/usr/local/bin/ztos-android'))


def binary_manifest(utf8=True):
    strings=['manifest','package','com.example.demo','application','label','Demo']
    offsets=[]; data=b''
    for value in strings:
        offsets.append(len(data))
        encoded=value.encode('utf-8' if utf8 else 'utf-16-le')
        data += (bytes([len(value),len(encoded)]) if utf8 else struct.pack('<H',len(value)))+encoded+(b'\0' if utf8 else b'\0\0')
    start=28+4*len(strings)
    pool=struct.pack('<HHIIIIII',1,28,start+len(data),len(strings),0,0x100 if utf8 else 0,start,0)
    pool+=struct.pack('<'+'I'*len(strings),*offsets)+data
    def element(tag,key,value):
        attr=struct.pack('<IIIHBBI',0xffffffff,key,value,8,0,3,value)
        ext=struct.pack('<IIHHHHHH',0xffffffff,tag,20,20,1,0,0,0)
        return struct.pack('<HHIII',0x102,16,16+len(ext)+len(attr),1,0xffffffff)+ext+attr
    content=pool+element(0,1,2)+element(3,4,5)
    return struct.pack('<HHI',3,8,8+len(content))+content

class LauncherTests(unittest.TestCase):
    def test_binary_manifest(self):
        for utf8 in (True,False):
            self.assertEqual(API['manifest_metadata'](binary_manifest(utf8)),('com.example.demo','Demo'))

    def test_invalid_manifest(self):
        for value in (b'bad',binary_manifest()[:-1],b'<manifest package="../../bad"/>'):
            with self.subTest(value=value[:8]),self.assertRaises(ValueError):
                API['manifest_metadata'](value)

    def test_install_and_update_menu(self):
        with tempfile.TemporaryDirectory() as temp,patch.dict(os.environ,{'XDG_DATA_HOME':temp}):
            apk=Path(temp)/'demo.apk'
            with zipfile.ZipFile(apk,'w') as archive:
                archive.writestr('AndroidManifest.xml',binary_manifest())
                archive.writestr('lib/arm64-v8a/libsample.so',b'fixture')
            metadata=API['install'](apk)
            self.assertEqual(metadata['abis'],['arm64-v8a'])
            self.assertEqual(Path(metadata['apk']).name, 'com.example.demo.apk')
            registry=API['installed']()
            self.assertEqual(list(registry),['com.example.demo@atl'])
            entry=Path(temp)/'applications/ztos-android-com.example.demo-atl.desktop'
            self.assertIn('Exec=ztos-android launch com.example.demo --runtime atl\n',entry.read_text())
            self.assertIn('Name=Demo\n',entry.read_text())
            API['install'](apk)
            self.assertEqual(len(API['installed']()),1)

    def test_dual_runtime_install_migration_and_uninstall(self):
        with tempfile.TemporaryDirectory() as temp, \
                patch.dict(os.environ, {'XDG_DATA_HOME': temp}), \
                patch('pathlib.Path.home', return_value=Path(temp)):
            apk = Path(temp)/'demo.apk'
            with zipfile.ZipFile(apk, 'w') as archive:
                archive.writestr('AndroidManifest.xml', binary_manifest())
            old = API['apk_metadata'](apk)
            old['apk'] = str(apk)
            registry_path = API['data_root']()/'applications.json'
            original = json.dumps({'com.example.demo': old})
            API['atomic_write'](registry_path, original)
            legacy = Path(temp)/'applications/ztos-android-com.example.demo.desktop'
            API['atomic_write'](legacy, '[Desktop Entry]\nIcon=old-icon\n')
            tiles = Path(temp)/'.config/ztmetro/tiles'
            API['atomic_write'](tiles, 'medium | accent | Demo | @old | ztos-android launch com.example.demo\n')
            migrated = API['installed']()
            self.assertEqual(registry_path.with_name('applications.json.pre-dual-backup').read_text(), original)
            self.assertEqual(migrated['com.example.demo@atl']['icon'], 'old-icon')
            self.assertIn('--runtime atl', tiles.read_text())
            self.assertFalse(legacy.exists())
            self.assertTrue(legacy.with_suffix('.desktop.pre-dual-backup').exists())
            API['install'](apk, 'waydroid')
            API['install'](apk, 'waydroid')
            registry = API['installed']()
            self.assertEqual(len(registry), 2)
            self.assertEqual(API['application'](registry, 'com.example.demo', 'atl')['apk'], str(apk))
            with self.assertRaisesRegex(ValueError, 'both runtimes'):
                API['application'](registry, 'com.example.demo')
            desktops = sorted((Path(temp)/'applications').glob('*.desktop'))
            self.assertEqual(len(desktops), 2)
            self.assertIn('Name=Demo (Waydroid)\n', desktops[1].read_text())
            self.assertIn('--runtime waydroid', desktops[1].read_text())
            with patch('sys.argv', ['ztos-android', 'uninstall', 'com.example.demo', '--runtime', 'waydroid']), \
                    patch('subprocess.run', side_effect=subprocess.CalledProcessError(1, 'remove')):
                with self.assertRaises(subprocess.CalledProcessError):
                    API['main']()
            self.assertEqual(API['installed'](), registry)
            self.assertTrue(all(path.exists() for path in desktops))
            self.assertTrue(Path(registry['com.example.demo@waydroid']['apk']).exists())
            with patch('sys.argv', ['ztos-android', 'uninstall', 'com.example.demo', '--runtime', 'waydroid']), \
                    patch('subprocess.run') as run:
                API['main']()
                run.assert_called_once_with(['ztos-waydroid-launch', '--remove', 'com.example.demo'], check=True)
            self.assertTrue(apk.exists())
            self.assertEqual(len(API['installed']()), 1)
            self.assertEqual(API['application'](API['installed'](), 'com.example.demo')['runtime'], 'atl')
            self.assertEqual(registry_path.with_name('applications.json.pre-dual-backup').read_text(), original)

    def test_dual_launch_explicit_runtime_and_list(self):
        from contextlib import redirect_stdout
        from io import StringIO
        registry = {
            'com.example.demo@atl': {'package': 'com.example.demo', 'name': 'Demo', 'runtime': 'atl', 'apk': '/demo.apk'},
            'com.example.demo@waydroid': {'package': 'com.example.demo', 'name': 'Demo', 'runtime': 'waydroid', 'apk': '/waydroid.apk'},
        }
        with patch.dict(API['main'].__globals__, installed=lambda: registry), \
                patch.dict(API['main'].__globals__, launch=lambda path, args: self.assertEqual((path, args), (Path('/demo.apk'), ['-w', '400']))), \
                patch('sys.argv', ['ztos-android', 'launch', 'com.example.demo', '--runtime', 'atl', '--', '-w', '400']):
            API['main']()
        output = StringIO()
        with patch.dict(API['main'].__globals__, installed=lambda: registry), \
                patch('sys.argv', ['ztos-android', 'list']), redirect_stdout(output):
            API['main']()
        self.assertEqual(output.getvalue().splitlines(), ['com.example.demo\tDemo\tatl', 'com.example.demo\tDemo\twaydroid'])

    def test_waydroid_launch_restores_existing_window_focus(self):
        with tempfile.TemporaryDirectory() as temp, \
                patch.dict(os.environ, {'XDG_RUNTIME_DIR': temp, 'XDG_STATE_HOME': temp}), \
                patch('sys.argv', ['ztos-android', 'launch', 'com.example.demo']), \
                patch.dict(API['main'].__globals__, installed=lambda: {
                    'com.example.demo': {'runtime': 'waydroid'}}), \
                patch('subprocess.run') as run:
            control = Path(temp)/'ztmetro.ctl'
            control.touch()
            run.return_value.returncode = 0
            API['main']()
            self.assertEqual(run.call_args.args[0], ['ztos-waydroid-launch', 'com.example.demo'])
            self.assertEqual(run.call_args.kwargs['stdin'], subprocess.DEVNULL)
            self.assertIn('exit=0', (Path(temp)/'ztos-android.log').read_text())
            self.assertEqual(control.read_text(), 'focus waydroid.com.example.demo\n')

    def test_waydroid_failure_keeps_diagnostic_and_does_not_focus(self):
        with tempfile.TemporaryDirectory() as temp, \
                patch.dict(os.environ, {'XDG_RUNTIME_DIR': temp, 'XDG_STATE_HOME': temp}), \
                patch('sys.argv', ['ztos-android', 'launch', 'com.example.demo']), \
                patch.dict(API['main'].__globals__, installed=lambda: {
                    'com.example.demo': {'runtime': 'waydroid'}}), \
                patch('subprocess.run') as run:
            control = Path(temp)/'ztmetro.ctl'
            control.touch()
            run.return_value.returncode = 1
            with self.assertRaisesRegex(ValueError, 'Waydroid launch failed'):
                API['main']()
            self.assertEqual(control.read_text(), '')
            self.assertIn('exit=1', (Path(temp)/'ztos-android.log').read_text())

    def test_obfuscated_manifest_icon(self):
        with tempfile.TemporaryDirectory() as temp, patch('pathlib.Path.home', return_value=Path(temp)):
            apk = Path(temp)/'demo.apk'
            actual = b'\x89PNG\r\n\x1a\n' + bytes(8) + struct.pack('>II', 48, 48)
            with zipfile.ZipFile(apk, 'w') as archive:
                archive.writestr('AndroidManifest.xml',
                    '<manifest xmlns:android="http://schemas.android.com/apk/res/android" '
                    'package="com.example.demo"><application android:icon="@7f010001"/></manifest>')
                archive.writestr('resources.arsc', b'resource fixture')
                archive.writestr('res/abc.png', actual)
                archive.writestr('res/launcher_unrelated.png', actual[:-8] + struct.pack('>II', 512, 512))
            with patch.dict(API['write_icon'].__globals__,
                            resource_strings=lambda data: {0x7f010001: 'res/abc.png'}):
                icon = API['write_icon'](apk, 'com.example.demo')
            self.assertEqual(Path(icon).read_bytes(), actual)

    def test_inspect_reports_existing_package(self):
        with tempfile.TemporaryDirectory() as temp, patch.dict(os.environ, {'XDG_DATA_HOME': temp}):
            apk=Path(temp)/'demo.apk'
            with zipfile.ZipFile(apk,'w') as archive:
                archive.writestr('AndroidManifest.xml',binary_manifest())
            metadata=API['apk_metadata'](apk)
            API['atomic_write'](API['data_root']()/'applications.json',
                                json.dumps({metadata['package']: metadata}))
            inspected, installed=API['inspect_apk'](apk)
            self.assertEqual(inspected['package'], metadata['package'])
            self.assertTrue(installed)

    def test_unsupported_native_abi(self):
        with tempfile.TemporaryDirectory() as temp:
            apk=Path(temp)/'arm32.apk'
            with zipfile.ZipFile(apk,'w') as archive:
                archive.writestr('AndroidManifest.xml',binary_manifest())
                archive.writestr('lib/armeabi-v7a/libsample.so',b'fixture')
            with self.assertRaisesRegex(ValueError,'arm64-v8a'):
                API['apk_metadata'](apk)

    def test_runtime_dependency_repair(self):
        ok = SimpleNamespace(returncode=0, stdout='')
        missing = SimpleNamespace(returncode=0, stdout='libart.so => not found')
        with patch('shutil.which', return_value='/usr/bin/android-translation-layer'), \
             patch('subprocess.run', side_effect=[missing, ok, ok]) as run:
            API['ensure_runtime']()
            self.assertIn('/usr/local/sbin/ztos-android-dependencies', run.call_args.args[0])
        with patch('shutil.which', return_value='/usr/bin/android-translation-layer'), \
             patch('subprocess.run', side_effect=[ok, ok]) as run:
            API['ensure_runtime']()
            self.assertEqual(run.call_count, 2)

    def test_dependency_repair_failure(self):
        ok = SimpleNamespace(returncode=0, stdout='')
        fail = SimpleNamespace(returncode=1, stdout='')
        with patch('shutil.which', return_value=None), \
             patch('subprocess.run', side_effect=[fail, fail]):
            with self.assertRaisesRegex(ValueError, 'dependency installation failed'):
                API['ensure_runtime']()

    def test_find_compiler_descendant_and_exit(self):
        parent = subprocess.Popen(['bash', '-c', 'exec -a dex2oat sleep 30 & wait'], start_new_session=True)
        try:
            for _ in range(100):
                found = API['compiler_children'](parent.pid)
                if found: break
                time.sleep(.01)
            self.assertEqual(len(found), 1)
            with patch('pathlib.Path.exists', return_value=False):
                self.assertEqual(API['compiler_children'](parent.pid), found)
            import signal
            os.killpg(parent.pid, signal.SIGTERM)
            parent.wait(timeout=3)
            self.assertEqual(API['compiler_children'](parent.pid), set())
        finally:
            if parent.poll() is None:
                os.killpg(parent.pid, 15)
                parent.wait(timeout=3)

    def test_launch_default_window_dimensions(self):
        with tempfile.TemporaryDirectory() as temp, \
             patch.dict(os.environ, {'XDG_DATA_HOME': temp, 'XDG_STATE_HOME': temp}), \
             patch('shutil.which', return_value='/usr/bin/android-translation-layer'), \
             patch('subprocess.run', return_value=SimpleNamespace(returncode=0, stdout='')), \
             patch('os.fork', return_value=1), \
             patch('os.waitpid', return_value=(1, 0)), \
             patch('os.dup2'), \
             patch('os.execvpe') as mock_exec:
            apk = Path(temp) / 'demo.apk'
            with zipfile.ZipFile(apk, 'w') as archive:
                archive.writestr('AndroidManifest.xml', binary_manifest())
                archive.writestr('lib/arm64-v8a/libsample.so', b'fixture')
            API['launch'](apk, [])
            mock_exec.assert_called_once()
            args = mock_exec.call_args[0][1]
            self.assertIn('--window-width', args)
            self.assertEqual(args[args.index('--window-width') + 1], '360')
            self.assertIn('--window-height', args)
            self.assertEqual(args[args.index('--window-height') + 1], '672')

    def test_launch_preserve_custom_dimensions(self):
        with tempfile.TemporaryDirectory() as temp, \
             patch.dict(os.environ, {'XDG_DATA_HOME': temp, 'XDG_STATE_HOME': temp}), \
             patch('shutil.which', return_value='/usr/bin/android-translation-layer'), \
             patch('subprocess.run', return_value=SimpleNamespace(returncode=0, stdout='')), \
             patch('os.fork', return_value=1), \
             patch('os.waitpid', return_value=(1, 0)), \
             patch('os.dup2'), \
             patch('os.execvpe') as mock_exec:
            apk = Path(temp) / 'demo.apk'
            with zipfile.ZipFile(apk, 'w') as archive:
                archive.writestr('AndroidManifest.xml', binary_manifest())
                archive.writestr('lib/arm64-v8a/libsample.so', b'fixture')
            API['launch'](apk, ['-w', '400', '--window-height=800'])
            mock_exec.assert_called_once()
            args = mock_exec.call_args[0][1]
            self.assertNotIn('--window-width', args)
            self.assertNotIn('--window-height', args)
            self.assertIn('-w', args)
            self.assertIn('--window-height=800', args)

if __name__=='__main__':
    unittest.main()
