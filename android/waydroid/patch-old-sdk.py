#!/usr/bin/env python3
"""Build a per-package Android version experiment and preserve boot-library reads.

Collect the exact live boot JARs on the phone, then rebuild only JARs referencing
the selected Build.VERSION fields. No APK or services.jar is modified.
"""
import argparse
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tarfile
import zipfile

VERSION = 'Landroid/os/Build$VERSION;'
PLATFORM = 'Landroid/os/ZtPlatformVersion;'
FIELDS = {
    'SDK_INT': ('I', 'ro.build.version.sdk', '23'),
    'SDK': ('Ljava/lang/String;', 'ro.build.version.sdk', '23'),
    'RELEASE': ('Ljava/lang/String;', 'ro.build.version.release', '6.0'),
    'RELEASE_OR_CODENAME': ('Ljava/lang/String;', 'ro.build.version.release_or_codename', '6.0'),
    'RELEASE_OR_PREVIEW_DISPLAY': ('Ljava/lang/String;', 'ro.build.version.release_or_preview_display', '6.0'),
    'SECURITY_PATCH': ('Ljava/lang/String;', 'ro.build.version.security_patch', '2015-10-01'),
}
REF = re.compile(re.escape(VERSION) + r'->(' + '|'.join(FIELDS) + r'):(I|Ljava/lang/String;)')
_spec = importlib.util.spec_from_file_location('hiddenapi', Path(__file__).with_name('dex-hiddenapi.py'))
hiddenapi = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hiddenapi)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def dex_fields(data):
    """Return relevant field IDs; no instruction decoding or false string matches."""
    count, start = struct.unpack_from('<II', data, 56)
    strings = []
    for i in range(count):
        offset, = struct.unpack_from('<I', data, start + 4 * i)
        while data[offset] & 128:
            offset += 1
        offset += 1
        strings.append(data[offset:data.index(0, offset)].decode('utf-8', 'replace'))
    count, start = struct.unpack_from('<II', data, 64)
    types = [strings[struct.unpack_from('<I', data, start + 4 * i)[0]] for i in range(count)]
    count, start = struct.unpack_from('<II', data, 80)
    result = set()
    for i in range(count):
        cls, typ, name = struct.unpack_from('<HHI', data, start + 8 * i)
        if types[cls] == VERSION and strings[name] in FIELDS:
            result.add(strings[name])
    return result


def collect(destination):
    pids = subprocess.check_output(['pgrep', '-f', '^zygote64$'], text=True).split()
    if len(pids) != 1:
        raise ValueError('Expected exactly one live Android zygote')
    env = dict(item.split(b'=', 1) for item in Path('/proc', pids[0], 'environ').read_bytes().split(b'\0') if b'=' in item)
    paths = env[b'BOOTCLASSPATH'].decode().split(':')
    report = []
    with tarfile.open(destination, 'w') as output:
        for path in paths:
            runtime = Path('/proc', pids[0], 'root') / path.lstrip('/')
            data = runtime.read_bytes()
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                fields = set().union(*(dex_fields(archive.read(n)) for n in archive.namelist() if n.endswith('.dex')))
            if not fields:
                continue
            image_path = 'system/' + path.lstrip('/') if path.startswith('/apex/') else path.lstrip('/')
            info = tarfile.TarInfo(image_path)
            info.size = len(data)
            info.mode = 0o644
            output.addfile(info, io.BytesIO(data))
            report.append({'path': image_path, 'sha256': digest(data), 'fields': sorted(fields)})
        info = tarfile.TarInfo('sources.json')
        data = json.dumps(report, indent=2).encode()
        info.size = len(data)
        output.addfile(info, io.BytesIO(data))
    os.chmod(destination, 0o644)
    print(json.dumps(report, indent=2))


def platform_smali():
    lines = ['.class public final ' + PLATFORM, '.super Ljava/lang/Object;', '']
    for name, (typ, _, _) in FIELDS.items():
        lines.append(f'.field public static final {name}:{typ}')
    lines += ['', '.method static constructor <clinit>()V', '    .registers 2']
    for name, (typ, prop, _) in FIELDS.items():
        lines += [f'    const-string v0, "{prop}"']
        if typ == 'I':
            lines += ['    const/4 v1, 0x0', '    invoke-static {v0, v1}, Landroid/os/SystemProperties;->getInt(Ljava/lang/String;I)I', '    move-result v0', f'    sput v0, {PLATFORM}->{name}:I']
        else:
            lines += ['    const-string v1, "unknown"', '    invoke-static {v0, v1}, Landroid/os/SystemProperties;->get(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;', '    move-result-object v0', f'    sput-object v0, {PLATFORM}->{name}:{typ}']
    lines += ['    return-void', '.end method', '']
    return '\n'.join(lines)


def hook_smali():
    lines = ['.class public final Landroid/app/ZtOldSdk;', '.super Ljava/lang/Object;', '',
             '.method public static apply(Landroid/app/ActivityThread$AppBindData;)V', '    .registers 8',
             '    iget-object v0, p0, Landroid/app/ActivityThread$AppBindData;->appInfo:Landroid/content/pm/ApplicationInfo;',
             '    if-eqz v0, :done',
             '    iget-object v1, v0, Landroid/content/pm/ApplicationInfo;->packageName:Ljava/lang/String;',
             '    const-string v0, "persist.ztos.oldsdk"',
             '    invoke-static {v0}, Landroid/os/SystemProperties;->get(Ljava/lang/String;)Ljava/lang/String;',
             '    move-result-object v0', '    const-string v2, ","',
             '    invoke-virtual {v0, v2}, Ljava/lang/String;->split(Ljava/lang/String;)[Ljava/lang/String;',
             '    move-result-object v2', '    const/4 v3, 0x0', '    array-length v4, v2',
             '    :loop', '    if-ge v3, v4, :done', '    aget-object v0, v2, v3',
             '    invoke-virtual {v0}, Ljava/lang/String;->trim()Ljava/lang/String;', '    move-result-object v0',
             '    invoke-virtual {v0, v1}, Ljava/lang/String;->equals(Ljava/lang/Object;)Z', '    move-result v0',
             '    if-nez v0, :apply', '    add-int/lit8 v3, v3, 0x1', '    goto :loop', '    :apply',
             f'    sget v4, {PLATFORM}->SDK_INT:I']
    for name, (typ, _, value) in FIELDS.items():
        if typ == 'I':
            lines += [f'    const/16 v0, {value}', f'    sput v0, {VERSION}->{name}:I']
        else:
            lines += [f'    const-string v0, "{value}"', f'    sput-object v0, {VERSION}->{name}:{typ}']
    lines += ['    new-instance v2, Ljava/lang/StringBuilder;',
              '    invoke-direct {v2}, Ljava/lang/StringBuilder;-><init>()V',
              '    const-string v0, "package="',
              '    invoke-virtual {v2, v0}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;',
              '    invoke-virtual {v2, v1}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;',
              '    const-string v0, " sdk=23 release=6.0 frameworkSdk="',
              '    invoke-virtual {v2, v0}, Ljava/lang/StringBuilder;->append(Ljava/lang/String;)Ljava/lang/StringBuilder;',
              '    invoke-virtual {v2, v4}, Ljava/lang/StringBuilder;->append(I)Ljava/lang/StringBuilder;',
              '    invoke-virtual {v2}, Ljava/lang/StringBuilder;->toString()Ljava/lang/String;',
              '    move-result-object v1', '    const-string v0, "ZtOldSdk"',
              '    invoke-static {v0, v1}, Landroid/util/Log;->i(Ljava/lang/String;Ljava/lang/String;)I',
              '    :done', '    return-void', '.end method', '']
    return '\n'.join(lines)


def aligned_jar(source, target, dex_data):
    with zipfile.ZipFile(source) as archive, zipfile.ZipFile(target, 'w') as output:
        for entry in archive.infolist():
            data = dex_data.get(entry.filename, archive.read(entry))
            if entry.filename.endswith('.dex'):
                entry.compress_type = zipfile.ZIP_STORED
                padding = (-(output.fp.tell() + 30 + len(entry.filename.encode()) + len(entry.extra))) & 3
                if padding:
                    entry.extra += struct.pack('<HH', 0xd935, padding) + bytes(padding)
            output.writestr(entry, data)
    with zipfile.ZipFile(target) as archive, open(target, 'rb') as raw:
        for entry in archive.infolist():
            if entry.filename.endswith('.dex'):
                raw.seek(entry.header_offset + 26)
                name, extra = struct.unpack('<HH', raw.read(4))
                if entry.compress_type != 0 or (entry.header_offset + 30 + name + extra) % 4:
                    raise ValueError('DEX compression/alignment regression')


def patch_jar(source, target, tools, work, main=False):
    work.mkdir(parents=True, exist_ok=True)
    result, rewrites = {}, 0
    with zipfile.ZipFile(source) as archive:
        if any(n.upper().endswith(('.SF', '.RSA', '.DSA', '.EC')) for n in archive.namelist()):
            raise ValueError('Refusing to rebuild a signed platform JAR')
        for name in archive.namelist():
            if not name.endswith('.dex'):
                continue
            dex = work / name
            dex.write_bytes(archive.read(name))
            tree = work / (name + '.smali')
            if tree.exists():
                shutil.rmtree(tree)
            subprocess.run(['java', '-jar', str(tools/'baksmali.jar'), 'd', '--api', '33', str(dex), '-o', str(tree)], check=True)
            changed = False
            for path in sorted(tree.rglob('*.smali')):
                text = path.read_text()
                if path.name == 'Build$VERSION.smali':
                    for field in FIELDS:
                        text, count = re.subn(r'(?m)^\.field public static final ' + field + ':', '.field public static ' + field + ':', text)
                        if count != 1:
                            raise ValueError(f'Expected exactly one final Build.VERSION.{field}')
                    (path.parent/'ZtPlatformVersion.smali').write_text(platform_smali())
                    changed = True
                else:
                    text, count = REF.subn(lambda m: PLATFORM + '->' + m[1] + ':' + m[2], text)
                    rewrites += count
                    changed |= bool(count)
                if path.name == 'ActivityThread.smali':
                    method = '.method private handleBindApplication(Landroid/app/ActivityThread$AppBindData;)V'
                    if text.count(method) != 1:
                        raise ValueError('Unexpected ActivityThread binding method')
                    start = text.index(method)
                    end = text.index('.end method', start)
                    body = text[start:end]
                    match = re.search(r'(?m)^    \.registers \d+\n', body)
                    if not match:
                        raise ValueError('Expected handleBindApplication registers directive')
                    pos = start + match.end()
                    text = text[:pos] + '\n    invoke-static/range {p1 .. p1}, Landroid/app/ZtOldSdk;->apply(Landroid/app/ActivityThread$AppBindData;)V\n' + text[pos:]
                    (path.parent/'ZtOldSdk.smali').write_text(hook_smali())
                    changed = True
                path.write_text(text)
            if changed:
                out = work/(name + '.patched')
                subprocess.run(['java', '-jar', str(tools/'smali.jar'), 'a', '--api', '33', str(tree), '-o', str(out)], check=True)
                result[name] = hiddenapi.restore(archive.read(name), out.read_bytes())
    if main and not any((work/(n+'.smali')/'android/app/ZtOldSdk.smali').exists() for n in result):
        raise ValueError('Missing framework bind hook')
    target.parent.mkdir(parents=True, exist_ok=True)
    aligned_jar(source, target, result)
    return rewrites


def bundle(source, output, tools, work):
    work.mkdir(parents=True, exist_ok=True)
    with tarfile.open(source) as archive:
        records = json.load(archive.extractfile('sources.json'))
        for record in records:
            path = Path(record['path'])
            if path.is_absolute() or '..' in path.parts or not str(path).startswith('system/'):
                raise ValueError('Unsafe image path')
            data = archive.extractfile(str(path)).read()
            if digest(data) != record['sha256']:
                raise ValueError('Input snapshot checksum mismatch')
            src = work/'source'/path
            src.parent.mkdir(parents=True, exist_ok=True)
            src.write_bytes(data)
            dst = work/'output'/path
            record['rewritten_references'] = patch_jar(src, dst, tools, work/'dex'/path.stem,
                                                     main=path.name == 'framework.jar')
            record['patched_sha256'] = digest(dst.read_bytes())
    manifest = {'version': 1, 'property': 'persist.ztos.oldsdk', 'packages': 'com.whatsapp', 'jars': records}
    manifest_path = work/'output/oldsdk-manifest.json'
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
    with tarfile.open(output, 'w') as archive:
        for record in records:
            archive.add(work/'output'/record['path'], arcname=record['path'])
        archive.add(manifest_path, arcname=manifest_path.name)
    print(json.dumps(manifest, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--collect', type=Path)
    parser.add_argument('--bundle', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--tools', type=Path)
    parser.add_argument('--work', type=Path)
    args = parser.parse_args()
    if args.collect:
        collect(args.collect)
    elif args.bundle and args.output and args.tools and args.work:
        bundle(args.bundle, args.output, args.tools.resolve(), args.work.resolve())
    else:
        parser.error('Use --collect TAR, or --bundle TAR --output TAR --tools DIR --work DIR')


if __name__ == '__main__':
    main()
