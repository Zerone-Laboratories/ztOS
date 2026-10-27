#!/usr/bin/env python3
"""Build the optional bridge and dual-runtime probe with an installed Android SDK."""
import argparse
import json
from pathlib import Path
import secrets
import shutil
import subprocess
import zipfile


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=Path.home() / 'Android/Sdk')
    parser.add_argument('--aapt2', type=Path)
    parser.add_argument('--endpoint', default='http://192.168.240.1:17892/notify')
    parser.add_argument('--work', type=Path, default=Path('build/waydroid/notification'))
    args = parser.parse_args()
    sdk = args.sdk.resolve()
    jars = sorted(sdk.glob('platforms/android-*/android.jar'))
    if not jars:
        parser.error('No SDK platform android.jar; pass --sdk')
    android = jars[-1]
    aapt = args.aapt2 or shutil.which('aapt2')
    if not aapt:
        matches = list((Path.home() / '.gradle/caches').glob('transforms-*/**/aapt2'))
        aapt = matches[0] if matches else None
    if not aapt:
        parser.error('No aapt2; pass --aapt2')
    candidates = list(sdk.glob('build-tools/*/d8')) + list(sdk.glob('cmdline-tools/*/bin/d8'))
    if not candidates:
        parser.error('No d8 in SDK')
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    work.chmod(0o700)
    token = work / 'notification-bridge.token'
    if not token.exists():
        with open(token, 'x', opener=lambda path, flags: __import__('os').open(path, flags, 0o600)) as out:
            out.write(secrets.token_hex(32) + '\n')
    if token.stat().st_mode & 0o077:
        parser.error('Existing token must have mode 600')
    config = work / 'BuildConfig.java'
    config.write_text('package org.ztos.notificationbridge;\nfinal class BuildConfig {\n'
                      'static final String ENDPOINT = ' + json.dumps(args.endpoint) + ';\n'
                      'static final String TOKEN = ' + json.dumps(token.read_text().strip()) + ';\n}\n')
    config.chmod(0o600)
    classes = work / 'classes'
    classes.mkdir(exist_ok=True)
    source = Path(__file__).resolve().parent
    run('javac', '-source', '8', '-target', '8', '-cp', android, '-d', classes,
        source / 'Listener.java', source / 'Probe.java', config)
    with zipfile.ZipFile(work / 'classes.jar', 'w') as archive:
        for path in classes.rglob('*.class'):
            archive.write(path, path.relative_to(classes))
    run(candidates[-1], '--min-api', '23', '--lib', android,
        '--output', work / 'dex.zip', work / 'classes.jar')
    run(aapt, 'link', '--manifest', source / 'AndroidManifest.xml', '-I', android,
        '-o', work / 'unsigned.apk')
    with zipfile.ZipFile(work / 'unsigned.apk', 'a') as apk, zipfile.ZipFile(work / 'dex.zip') as dex:
        apk.writestr('classes.dex', dex.read('classes.dex'))
    key = work / 'bridge.jks'
    if not key.exists():
        # Local test signing key. Keep stable for upgrades without deleting app data.
        run('keytool', '-genkeypair', '-alias', 'bridge', '-keystore', key,
            '-storepass', 'android', '-keypass', 'android', '-keyalg', 'RSA',
            '-validity', '3650', '-dname', 'CN=ztOS local notification bridge', '-noprompt')
    run('jarsigner', '-keystore', key, '-storepass', 'android', '-signedjar',
        work / 'notification-bridge.apk', work / 'unsigned.apk', 'bridge')
    print(work / 'notification-bridge.apk')


if __name__ == '__main__':
    main()
