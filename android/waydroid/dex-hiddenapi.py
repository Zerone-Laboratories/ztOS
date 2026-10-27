"""Retain Android 13 hidden-API flags when rebuilding with the repo's old smali.

Hidden-API flags follow class-data member order, which changes on assembly.
Map them by full member identity, then emit a new section and DEX map.
"""
import hashlib
import struct
import zlib


def uleb(data, offset):
    value = shift = 0
    while True:
        byte = data[offset]
        offset += 1
        value |= (byte & 127) << shift
        if byte < 128:
            return value, offset
        shift += 7


def encoded(value):
    out = bytearray()
    while value >= 128:
        out.append((value & 127) | 128)
        value >>= 7
    out.append(value)
    return out


def map_items(data):
    offset, = struct.unpack_from('<I', data, 52)
    count, = struct.unpack_from('<I', data, offset)
    return [struct.unpack_from('<HHII', data, offset + 4 + 12*i) for i in range(count)]


def members(data):
    count, start = struct.unpack_from('<II', data, 56)
    strings = []
    for i in range(count):
        pos, = struct.unpack_from('<I', data, start + 4*i)
        _, pos = uleb(data, pos)
        strings.append(data[pos:data.index(0, pos)].decode('utf-8', 'replace'))
    count, start = struct.unpack_from('<II', data, 64)
    types = [strings[struct.unpack_from('<I', data, start + 4*i)[0]] for i in range(count)]
    count, start = struct.unpack_from('<II', data, 72)
    protos = []
    for i in range(count):
        _, ret, args = struct.unpack_from('<III', data, start + 12*i)
        params = ()
        if args:
            size, = struct.unpack_from('<I', data, args)
            params = tuple(types[struct.unpack_from('<H', data, args + 4 + 2*j)[0]] for j in range(size))
        protos.append((types[ret], params))
    count, start = struct.unpack_from('<II', data, 80)
    fields = []
    for i in range(count):
        cls, typ, name = struct.unpack_from('<HHI', data, start + 8*i)
        fields.append(('field', types[cls], strings[name], types[typ]))
    count, start = struct.unpack_from('<II', data, 88)
    methods = []
    for i in range(count):
        cls, proto, name = struct.unpack_from('<HHI', data, start + 8*i)
        methods.append(('method', types[cls], strings[name], protos[proto]))
    count, start = struct.unpack_from('<II', data, 96)
    result = []
    for i in range(count):
        offset, = struct.unpack_from('<I', data, start + 32*i + 24)
        entries = []
        if offset:
            counts = []
            for _ in range(4):
                size, offset = uleb(data, offset)
                counts.append(size)
            for group, size in enumerate(counts):
                index = 0
                for _ in range(size):
                    delta, offset = uleb(data, offset)
                    _, offset = uleb(data, offset)  # access flags
                    index += delta
                    if group >= 2:
                        _, offset = uleb(data, offset)  # code offset
                    entries.append((fields if group < 2 else methods)[index])
        result.append(entries)
    return result


def flags(data):
    classes = members(data)
    section = next((entry[3] for entry in map_items(data) if entry[0] == 0xf000), None)
    result = {}
    for i, entries in enumerate(classes):
        offset = 0
        if section is not None:
            offset, = struct.unpack_from('<I', data, section + 4 + 4*i)
        pos = section + offset if offset else 0
        for key in entries:
            value = 0
            if pos:
                value, pos = uleb(data, pos)
            result[key] = value
    return result


def restore(source, rebuilt):
    if not any(item[0] == 0xf000 for item in map_items(source)):
        return rebuilt
    original = flags(source)
    classes = members(rebuilt)
    present = {key for entries in classes for key in entries}
    missing = original.keys() - present
    if missing:
        raise ValueError(f'Assembly lost {len(missing)} original members')
    section = bytearray(4 + 4*len(classes))
    for i, entries in enumerate(classes):
        values = [original.get(key, 0) for key in entries]
        if any(values):
            struct.pack_into('<I', section, 4 + 4*i, len(section))
            for value in values:
                section += encoded(value)
    struct.pack_into('<I', section, 0, len(section))
    old_map, = struct.unpack_from('<I', rebuilt, 52)
    old_items = map_items(rebuilt)
    if old_map + 4 + 12*len(old_items) != len(rebuilt):
        raise ValueError('Expected assembler map at end of DEX')
    result = bytearray(rebuilt[:old_map])
    result += bytes((-len(result)) & 3)
    hidden_offset = len(result)
    result += section
    result += bytes((-len(result)) & 3)
    map_offset = len(result)
    items = [item for item in old_items if item[0] not in (0x1000, 0xf000)]
    items += [(0xf000, 0, 1, hidden_offset), (0x1000, 0, 1, map_offset)]
    items.sort(key=lambda item: item[3])
    result += struct.pack('<I', len(items))
    for item in items:
        result += struct.pack('<HHII', *item)
    struct.pack_into('<I', result, 52, map_offset)
    struct.pack_into('<I', result, 32, len(result))
    data_offset, = struct.unpack_from('<I', result, 108)
    struct.pack_into('<I', result, 104, len(result) - data_offset)
    result[12:32] = hashlib.sha1(result[32:]).digest()
    struct.pack_into('<I', result, 8, zlib.adler32(result[12:]) & 0xffffffff)
    actual = flags(result)
    if any(actual[key] != value for key, value in original.items()):
        raise ValueError('Hidden-API member flags changed')
    return bytes(result)
