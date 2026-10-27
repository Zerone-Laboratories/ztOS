#!/usr/bin/env python3
"""Build a non-test-only APK using the retained demo probe's manifest and icon."""
import argparse
from pathlib import Path
import struct
import subprocess
import zipfile


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def regular_manifest(data):
    data = bytearray(data)
    position = 8
    resources = []
    changed = 0
    while position < len(data):
        kind, header, size = struct.unpack_from('<HHI', data, position)
        if kind == 0x180:
            resources = list(struct.unpack_from('<' + 'I' * ((size-header)//4), data, position+header))
        elif kind == 0x102:
            start, stride, count = struct.unpack_from('<HHH', data, position+24)
            for index in range(count):
                offset = position+16+start+index*stride
                name = struct.unpack_from('<I', data, offset+4)[0]
                if name < len(resources) and resources[name] == 0x01010272:
                    struct.pack_into('<I', data, offset+8, 0xffffffff)
                    struct.pack_into('<I', data, offset+16, 0)
                    changed += 1
        position += size
    if changed != 1:
        raise ValueError(f'Expected one android:testOnly attribute, found {changed}')
    return data


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--base-apk', required=True, type=Path)
    parser.add_argument('--android-jar', required=True, type=Path)
    parser.add_argument('--d8', required=True)
    parser.add_argument('--work', required=True, type=Path)
    args = parser.parse_args()
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    classes = work/'classes'
    classes.mkdir(exist_ok=True)
    run('javac', '-source', '8', '-target', '8', '-cp', args.android_jar,
        '-d', classes, Path(__file__).parent/'probe/MainActivity.java')
    with zipfile.ZipFile(work/'classes.jar', 'w') as archive:
        for path in classes.rglob('*.class'):
            archive.write(path, path.relative_to(classes))
    run(args.d8, '--min-api', '23', '--lib', args.android_jar,
        '--output', work/'dex.zip', work/'classes.jar')
    with zipfile.ZipFile(args.base_apk) as base, zipfile.ZipFile(work/'dex.zip') as dex, \
            zipfile.ZipFile(work/'unsigned.apk', 'w', zipfile.ZIP_DEFLATED) as output:
        for entry in base.infolist():
            if entry.filename.startswith('META-INF/') or entry.filename.endswith('.dex'):
                continue
            data = base.read(entry)
            if entry.filename == 'AndroidManifest.xml':
                data = regular_manifest(data)
            output.writestr(entry, data)
        output.writestr('classes.dex', dex.read('classes.dex'))
    if not (work/'probe.jks').exists():
        # Disposable local test key; never used for distribution.
        run('keytool', '-genkeypair', '-alias', 'probe', '-keystore', work/'probe.jks',
            '-storepass', 'android', '-keypass', 'android', '-keyalg', 'RSA',
            '-validity', '30', '-dname', 'CN=ztOS Phase 2 probe', '-noprompt')
    run('jarsigner', '-keystore', work/'probe.jks', '-storepass', 'android',
        '-signedjar', work/'phase2-probe.apk', work/'unsigned.apk', 'probe')
    print(work/'phase2-probe.apk')


if __name__ == '__main__':
    main()
