#!/usr/bin/env python3
"""Disable unsupported device-ID attestation in the AndroidKeyStore API getter."""
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib


SOURCE_SHA256 = '84448fd7d7b793a53fb25b55c5decfeadb0736f29ab50fa0c6bd8eb0b4980cb7'
LEGACY_PATCHED_SHA256 = '00eec027c08f22c0c3095866d27cb33877881e9631a9eb35d1d080cced07013c'
ALIGNED_PATCHED_SHA256 = 'fa6d1a28aa9cbfc2ae7c441fe695c84e0f4dbe9b004488063b1dcb42a60b8bbc'
CLASS = 'Landroid/security/keystore/KeyGenParameterSpec;'
METHOD = 'isDevicePropertiesAttestationIncluded'
CHALLENGE_METHOD = 'getAttestationChallenge'
ORIGINAL = (0x1055, 0x1d9f, 0x000f)  # iget-boolean v0, v1, field; return v0
PATCHED = (0x0000, 0x0012, 0x000f)   # nop; const/4 v0, 0; return v0
CHALLENGE_ORIGINAL = (0x1054, 0x1d97, 0x1071, 0x1ce2, 0x0000, 0x000c, 0x0011)
CHALLENGE_PATCHED = (0x0012, 0x0011, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000)


def uleb(data, offset):
    value = shift = 0
    while True:
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7f) << shift
        if byte < 0x80:
            return value, offset
        shift += 7


def method_code(data, method_name=METHOD):
    string_size, string_off = struct.unpack_from('<II', data, 56)
    strings = []
    for index in range(string_size):
        offset = struct.unpack_from('<I', data, string_off + index * 4)[0]
        _, offset = uleb(data, offset)
        end = data.index(0, offset)
        strings.append(bytes(data[offset:end]).decode('utf-8', 'replace'))

    type_size, type_off = struct.unpack_from('<II', data, 64)
    types = [struct.unpack_from('<I', data, type_off + index * 4)[0]
             for index in range(type_size)]
    method_size, method_off = struct.unpack_from('<II', data, 88)
    methods = [struct.unpack_from('<HHI', data, method_off + index * 8)
               for index in range(method_size)]
    class_size, class_off = struct.unpack_from('<II', data, 96)
    matches = []

    for index in range(class_size):
        definition = class_off + index * 32
        class_index = struct.unpack_from('<I', data, definition)[0]
        class_data_off = struct.unpack_from('<I', data, definition + 24)[0]
        if not class_data_off or strings[types[class_index]] != CLASS:
            continue
        counts = []
        offset = class_data_off
        for _ in range(4):
            count, offset = uleb(data, offset)
            counts.append(count)
        for count in counts[:2]:
            method_index = 0
            for _ in range(count):
                diff, offset = uleb(data, offset)
                _, offset = uleb(data, offset)
                method_index += diff
        for count in counts[2:]:
            method_index = 0
            for _ in range(count):
                diff, offset = uleb(data, offset)
                _, offset = uleb(data, offset)
                code_off, offset = uleb(data, offset)
                method_index += diff
                _, _, name_index = methods[method_index]
                if strings[name_index] == method_name:
                    matches.append(code_off)

    if len(matches) != 1 or matches[0] == 0:
        raise ValueError(f'Expected one implementation of {CLASS}->{method_name}, found {len(matches)}')
    code_off = matches[0]
    insns_size = struct.unpack_from('<I', data, code_off + 12)[0]
    required_size = len(CHALLENGE_ORIGINAL) if method_name == CHALLENGE_METHOD else len(ORIGINAL)
    if insns_size < required_size:
        raise ValueError('Attestation getter has an unexpected short body.')
    return code_off + 16


def read_units(data, offset):
    return struct.unpack_from('<3H', data, offset)


def update_dex_integrity(data):
    data[12:32] = hashlib.sha1(data[32:]).digest()
    struct.pack_into('<I', data, 8, zlib.adler32(data[12:]) & 0xffffffff)


def patch_dex(data):
    offset = method_code(data)
    current = read_units(data, offset)
    if current == ORIGINAL:
        struct.pack_into('<3H', data, offset, *PATCHED)
        update_dex_integrity(data)
    elif current != PATCHED:
        raise ValueError(f'Unexpected attestation getter instructions: {current!r}')
    challenge_offset = method_code(data, CHALLENGE_METHOD)
    challenge_current = struct.unpack_from('<7H', data, challenge_offset)
    if challenge_current == CHALLENGE_ORIGINAL:
        struct.pack_into('<7H', data, challenge_offset, *CHALLENGE_PATCHED)
        update_dex_integrity(data)
    elif challenge_current != CHALLENGE_PATCHED:
        raise ValueError(f'Unexpected challenge getter instructions: {challenge_current!r}')
    return data


def patch_jar(source, destination):
    source = Path(source)
    destination = Path(destination)
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    with zipfile.ZipFile(source) as archive:
        entries = archive.infolist()
        dex_names = [entry.filename for entry in entries
                     if entry.filename.startswith('classes') and entry.filename.endswith('.dex')]
        if any(
                name.upper().endswith(('.SF', '.RSA', '.DSA', '.EC')) for name in archive.namelist()):
            raise ValueError('Refusing to alter a signed framework.jar.')
        matches = []
        for name in dex_names:
            content = bytearray(archive.read(name))
            try:
                offset = method_code(content)
                challenge_offset = method_code(content, CHALLENGE_METHOD)
            except ValueError as error:
                if 'found 0' in str(error):
                    continue
                raise
            current = read_units(content, offset)
            if current not in (ORIGINAL, PATCHED):
                raise ValueError(f'Unexpected attestation getter instructions in {name}: {current!r}')
            challenge_current = struct.unpack_from('<7H', content, challenge_offset)
            if challenge_current not in (CHALLENGE_ORIGINAL, CHALLENGE_PATCHED):
                raise ValueError(f'Unexpected challenge getter instructions in {name}: {challenge_current!r}')
            matches.append((name, content, offset, current, challenge_offset, challenge_current))
        if len(matches) != 1:
            raise ValueError(f'Expected one target DEX method, found {len(matches)}')

        target_name, target_data, target_offset, current, challenge_offset, challenge_current = matches[0]
        already_aligned = all(
            entry.compress_type == zipfile.ZIP_STORED
            and local_data_offset(source, entry) % 4 == 0
            for entry in entries if entry.filename in dex_names)
        if current == PATCHED and challenge_current == CHALLENGE_PATCHED and already_aligned:
            shutil.copy2(source, destination)
            return
        if digest not in (SOURCE_SHA256, LEGACY_PATCHED_SHA256, ALIGNED_PATCHED_SHA256):
            raise ValueError(f'Unknown framework.jar SHA256: {digest}')
        if current == ORIGINAL:
            struct.pack_into('<3H', target_data, target_offset, *PATCHED)
            update_dex_integrity(target_data)
        if challenge_current == CHALLENGE_ORIGINAL:
            struct.pack_into('<7H', target_data, challenge_offset, *CHALLENGE_PATCHED)
            update_dex_integrity(target_data)

        destination.parent.mkdir(parents=True, exist_ok=True)
        fd, temporary = tempfile.mkstemp(prefix=destination.name + '.', dir=destination.parent)
        os.close(fd)
        try:
            with zipfile.ZipFile(temporary, 'w') as output:
                for entry in entries:
                    content = target_data if entry.filename == target_name else archive.read(entry.filename)
                    if entry.filename in dex_names:
                        entry.compress_type = zipfile.ZIP_STORED
                        # zipalign-compatible extra field; preserve other entries' compression.
                        name_length = len(entry.filename.encode('utf-8'))
                        offset = output.fp.tell() + 30 + name_length + len(entry.extra)
                        padding = (-offset) & 3
                        if padding:
                            entry.extra += struct.pack('<HH', 0xD935, padding) + bytes(padding)
                    output.writestr(entry, content)
            os.replace(temporary, destination)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)

    with zipfile.ZipFile(destination) as archive:
        data = bytearray(archive.read(target_name))
        offset = method_code(data)
        if read_units(data, offset) != PATCHED:
            raise ValueError('Patched getter verification failed.')
        challenge_offset = method_code(data, CHALLENGE_METHOD)
        if struct.unpack_from('<7H', data, challenge_offset) != CHALLENGE_PATCHED:
            raise ValueError('Attestation challenge stripping verification failed.')
    with zipfile.ZipFile(destination) as archive:
        for entry in archive.infolist():
            if entry.filename.startswith('classes') and entry.filename.endswith('.dex'):
                if entry.compress_type != zipfile.ZIP_STORED:
                    raise ValueError(f'{entry.filename} is not stored uncompressed')
                with open(destination, 'rb') as raw:
                    raw.seek(entry.header_offset + 26)
                    name_length, extra_length = struct.unpack('<HH', raw.read(4))
                    payload_offset = entry.header_offset + 30 + name_length + extra_length
                if payload_offset % 4:
                    raise ValueError(f'{entry.filename} is not four-byte aligned')


def local_data_offset(path, entry):
    with open(path, 'rb') as raw:
        raw.seek(entry.header_offset + 26)
        name_length, extra_length = struct.unpack('<HH', raw.read(4))
    return entry.header_offset + 30 + name_length + extra_length


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--old-sdk':
        subprocess.run([sys.executable, str(Path(__file__).with_name('patch-old-sdk.py')),
                        *sys.argv[2:]], check=True)
        return
    if len(sys.argv) != 3:
        raise SystemExit('Usage: patch-id-attestation.py SOURCE_FRAMEWORK_JAR OUTPUT_FRAMEWORK_JAR')
    patch_jar(sys.argv[1], sys.argv[2])
    print(hashlib.sha256(Path(sys.argv[2]).read_bytes()).hexdigest())


if __name__ == '__main__':
    main()
