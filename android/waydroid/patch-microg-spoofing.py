#!/usr/bin/env python3
"""Enable LineageOS's existing, signer-restricted microG spoofing on user builds.

This image already implements signature replacement for GmsCore and Phonesky.
Only remove the debuggable-build gate; retain package, signer and Google-cert checks.
No DEX instructions move. All DEX payloads remain stored and four-byte aligned.
"""
import hashlib
import importlib.util
from pathlib import Path
import struct
import sys
import zipfile

spec = importlib.util.spec_from_file_location('dex', Path(__file__).with_name('patch-id-attestation.py'))
dex = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dex)
dex.CLASS = 'Lcom/android/server/pm/ComputerEngine;'
dex.METHOD = 'isMicrogSigned'
STOCK = '2634ae4c9f2d8fcb9e3cf269606bd8f0e99ae8b53dd04beaa409dde7842a72cb'
PATCH = (0x1012, 0, 0, 0)  # const/4 v0, 1; nop; nop; nop


def patch(source, target):
    source, target = Path(source), Path(target)
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    with zipfile.ZipFile(source) as archive:
        changes = {}
        for entry in archive.infolist():
            if not (entry.filename.startswith('classes') and entry.filename.endswith('.dex')):
                continue
            data = bytearray(archive.read(entry))
            try:
                offset = dex.method_code(data, 'isMicrogSigned')
            except ValueError as error:
                if 'found 0' in str(error):
                    continue
                raise
            units = struct.unpack_from('<4H', data, offset)
            if units != PATCH:
                if digest != STOCK or units[0] != 0x0071 or units[2:] != (0, 0x000a):
                    raise ValueError(f'Unexpected services.jar or gate instructions: {digest}, {units}')
                struct.pack_into('<4H', data, offset, *PATCH)
                dex.update_dex_integrity(data)
            changes[entry.filename] = data
        if len(changes) != 1:
            raise ValueError(f'Expected one microG gate; found {len(changes)}')
        with zipfile.ZipFile(target, 'w') as output:
            for entry in archive.infolist():
                data = changes.get(entry.filename, archive.read(entry))
                if entry.filename.startswith('classes') and entry.filename.endswith('.dex'):
                    entry.compress_type = zipfile.ZIP_STORED
                    padding = (-(output.fp.tell() + 30 + len(entry.filename.encode()) + len(entry.extra))) & 3
                    if padding:
                        entry.extra += struct.pack('<HH', 0xd935, padding) + bytes(padding)
                output.writestr(entry, data)
    with zipfile.ZipFile(target) as archive:
        for entry in archive.infolist():
            if entry.filename.startswith('classes') and entry.filename.endswith('.dex'):
                assert entry.compress_type == zipfile.ZIP_STORED
                assert dex.local_data_offset(target, entry) % 4 == 0
    print(hashlib.sha256(target.read_bytes()).hexdigest(), target)


if __name__ == '__main__':
    patch(*sys.argv[1:])
