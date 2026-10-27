#!/usr/bin/env python3
"""Fetch pinned official microG APKs and prepare an arm64 privileged-app payload."""
import hashlib
from pathlib import Path
import shutil
import struct
import sys
import tarfile
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

APKS = {
    'GmsCore': ('com.google.android.gms', 'https://github.com/microg/GmsCore/releases/download/v0.3.17.252432/com.google.android.gms-252432034.apk', 'e5e6cba078bdec812b5450599350e5c1a12c8b7438da5bf77123af170f8c1a35'),
    'GsfProxy': ('com.google.android.gsf', 'https://github.com/microg/GsfProxy/releases/download/v0.1.0/GsfProxy.apk', '86891b174301f06a1c84187b545a0a2a57044c6b768f3e84e865908743349692'),
    'FakeStore': ('com.android.vending', 'https://github.com/microg/FakeStore/releases/download/v0.2.1/com.android.vending-83700037.apk', 'ab3e5396956a80a1a3ebc7c1953da008d69f694f133bdd0cbc95e0e8e19ecda0'),
}


def manifest_permissions(data):
    """Read requested permissions from Android binary XML without external tools."""
    strings = []
    permissions = set()
    offset = 8
    while offset < len(data):
        kind, header, size = struct.unpack_from('<HHI', data, offset)
        if kind == 1:
            count, _, flags, start, _ = struct.unpack_from('<5I', data, offset + 8)
            def length(pos, utf8):
                value = data[pos] if utf8 else struct.unpack_from('<H', data, pos)[0]
                step, mask = (1, 0x80) if utf8 else (2, 0x8000)
                pos += step
                if value & mask:
                    tail = data[pos] if utf8 else struct.unpack_from('<H', data, pos)[0]
                    value = ((value & (mask - 1)) << (7 if utf8 else 15)) | tail
                    pos += step
                return value, pos
            for i in range(count):
                pos = offset + start + struct.unpack_from('<I', data, offset + header + i * 4)[0]
                if flags & 0x100:
                    _, pos = length(pos, True)
                    n, pos = length(pos, True)
                    strings.append(data[pos:pos+n].decode('utf-8'))
                else:
                    n, pos = length(pos, False)
                    strings.append(data[pos:pos+n*2].decode('utf-16le'))
        elif kind == 0x102:
            name = strings[struct.unpack_from('<I', data, offset + 20)[0]]
            attr_start, attr_size, count = struct.unpack_from('<3H', data, offset + 24)
            if name.startswith('uses-permission'):
                for i in range(count):
                    pos = offset + 16 + attr_start + i * attr_size
                    _, attr_name, raw, _, _, value_type, value = struct.unpack_from('<IIIHBBI', data, pos)
                    if strings[attr_name] == 'name' and value_type == 3:
                        permissions.add(strings[value])
        offset += size
    return permissions


def prepare(work):
    work = Path(work)
    work.mkdir(parents=True, exist_ok=True)
    root = work / 'payload/system'
    for folder, (package, url, digest) in APKS.items():
        apk = work / (folder + '.apk')
        if not apk.exists():
            urllib.request.urlretrieve(url, apk)
        if hashlib.sha256(apk.read_bytes()).hexdigest() != digest:
            raise ValueError(f'APK checksum mismatch: {apk}')
        dest = root / 'priv-app' / ('MicroG' + folder)
        dest.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(apk, dest / (folder + '.apk'))
        with zipfile.ZipFile(apk) as archive:
            permissions = manifest_permissions(archive.read('AndroidManifest.xml'))
            for name in archive.namelist():
                if name.startswith('lib/arm64-v8a/') and name.endswith('.so'):
                    lib = dest / 'lib/arm64' / Path(name).name
                    lib.parent.mkdir(parents=True, exist_ok=True)
                    lib.write_bytes(archive.read(name))
        xml = ET.Element('permissions')
        grant = ET.SubElement(xml, 'privapp-permissions', {'package': package})
        for permission in sorted(permissions):
            if permission.startswith('android.permission.'):
                ET.SubElement(grant, 'permission', {'name': permission})
        path = root / 'etc/permissions' / (package + '.microg.xml')
        path.parent.mkdir(parents=True, exist_ok=True)
        ET.indent(xml)
        ET.ElementTree(xml).write(path, encoding='utf-8', xml_declaration=True)
        print(package, len(permissions), 'requested permissions')
    xml = ET.Element('permissions')
    for package, _, _ in APKS.values():
        ET.SubElement(xml, 'allow-in-power-save', {'package': package})
        ET.SubElement(xml, 'allow-in-data-usage-save', {'package': package})
    config = root / 'etc/sysconfig/ztos-microg.xml'
    config.parent.mkdir(parents=True, exist_ok=True)
    ET.indent(xml)
    ET.ElementTree(xml).write(config, encoding='utf-8', xml_declaration=True)
    def normalize(info):
        info.uid = info.gid = info.mtime = 0
        info.uname = info.gname = 'root'
        info.mode = 0o755 if info.isdir() else 0o644
        return info
    with tarfile.open(work / 'payload.tar', 'w') as archive:
        archive.add(root, arcname='system', filter=normalize)
    print(work / 'payload.tar')


if __name__ == '__main__':
    prepare(sys.argv[1])
