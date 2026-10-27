#!/usr/bin/env python3
"""Desugar core-oj lambdas in ART's host DEX without changing platform APIs."""

import argparse
import os
import struct
import subprocess
import tempfile
import zipfile
from pathlib import Path


def read_dex_classes(dex):
    if len(dex) < 0x70 or not dex.startswith(b"dex\n"):
        raise ValueError("input is not a valid DEX file")

    string_ids_size, string_ids_off = struct.unpack_from("<II", dex, 0x38)
    type_ids_size, type_ids_off = struct.unpack_from("<II", dex, 0x40)
    class_defs_size, class_defs_off = struct.unpack_from("<II", dex, 0x60)
    if not string_ids_size or not type_ids_size:
        raise ValueError("DEX has no string or type identifiers")
    if string_ids_off + string_ids_size * 4 > len(dex):
        raise ValueError("DEX string table exceeds file bounds")
    if type_ids_off + type_ids_size * 4 > len(dex):
        raise ValueError("DEX type table exceeds file bounds")
    if class_defs_off + class_defs_size * 32 > len(dex):
        raise ValueError("DEX class table exceeds file bounds")

    string_cache = {}

    def string_at(index):
        if index >= string_ids_size:
            raise ValueError("DEX string index exceeds table")
        if index not in string_cache:
            data_off = struct.unpack_from("<I", dex, string_ids_off + index * 4)[0]
            if data_off >= len(dex):
                raise ValueError("DEX string data exceeds file bounds")
            pos = data_off
            while pos < len(dex) and dex[pos] & 0x80:
                pos += 1
            if pos >= len(dex):
                raise ValueError("unterminated DEX string length")
            pos += 1
            end = dex.find(b"\0", pos)
            if end < 0:
                raise ValueError("unterminated DEX string")
            string_cache[index] = dex[pos:end].decode("utf-8")
        return string_cache[index]

    classes = set()
    for i in range(class_defs_size):
        class_index = struct.unpack_from("<I", dex, class_defs_off + i * 32)[0]
        if class_index >= type_ids_size:
            raise ValueError("DEX class type index exceeds table")
        descriptor_index = struct.unpack_from("<I", dex, type_ids_off + class_index * 4)[0]
        descriptor = string_at(descriptor_index)
        if not descriptor.startswith("L") or not descriptor.endswith(";"):
            raise ValueError(f"invalid class descriptor: {descriptor!r}")
        classes.add(descriptor[1:-1] + ".class")
    if len(classes) != class_defs_size:
        raise ValueError("DEX contains duplicate class definitions")
    return classes


def dex_map_counts(dex):
    map_off = struct.unpack_from("<I", dex, 0x34)[0]
    if map_off + 4 > len(dex):
        raise ValueError("DEX map list exceeds file bounds")
    item_count = struct.unpack_from("<I", dex, map_off)[0]
    if map_off + 4 + item_count * 12 > len(dex):
        raise ValueError("DEX map entries exceed file bounds")
    return {
        struct.unpack_from("<H", dex, map_off + 4 + i * 12)[0]: struct.unpack_from(
            "<I", dex, map_off + 8 + i * 12
        )[0]
        for i in range(item_count)
    }


def classes_from_hostdex(path):
    with zipfile.ZipFile(path) as archive:
        try:
            dex = archive.read("classes.dex")
        except KeyError as exc:
            raise ValueError(f"{path} has no classes.dex") from exc
    return read_dex_classes(dex)


def extract_class_set(source_jar, classes, output_jar):
    seen = set()
    with zipfile.ZipFile(source_jar) as source, zipfile.ZipFile(output_jar, "w") as output:
        for info in source.infolist():
            if info.filename in classes:
                output.writestr(info, source.read(info.filename))
                seen.add(info.filename)
    missing = classes - seen
    if missing:
        example = sorted(missing)[:5]
        raise ValueError(f"{len(missing)} DEX classes missing from core-all_classes.jar: {example}")
    if len(seen) != len(classes):
        raise ValueError("class extraction count mismatch")


def rewrite_hostdex(path, dex_path, expected_classes):
    temp_path = path.with_suffix(path.suffix + ".new")
    with zipfile.ZipFile(path, "r") as source:
        original_names = source.namelist()
        if original_names.count("classes.dex") != 1:
            raise ValueError(f"{path} must contain exactly one classes.dex")
        with zipfile.ZipFile(temp_path, "w") as output:
            output.comment = source.comment
            for info in source.infolist():
                data = dex_path.read_bytes() if info.filename == "classes.dex" else source.read(info.filename)
                output.writestr(info, data)

    with zipfile.ZipFile(temp_path) as rewritten:
        dex = rewritten.read("classes.dex")
    actual_classes = read_dex_classes(dex)
    missing = expected_classes - actual_classes
    if missing:
        temp_path.unlink()
        raise ValueError(f"D8 removed {len(missing)} original core-oj classes")
    map_counts = dex_map_counts(dex)
    if map_counts.get(0x0007, 0) or map_counts.get(0x0008, 0):
        temp_path.unlink()
        raise ValueError("D8 output still contains call-site or method-handle items")
    os.replace(temp_path, path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--core-all", required=True, type=Path)
    parser.add_argument("--core-oj-hostdex", required=True, type=Path)
    parser.add_argument("--core-libart-hostdex", required=True, type=Path)
    parser.add_argument("--r8", required=True, type=Path)
    parser.add_argument("--java", default="java")
    args = parser.parse_args()

    oj_classes = classes_from_hostdex(args.core_oj_hostdex)
    libart_classes = classes_from_hostdex(args.core_libart_hostdex)
    if oj_classes & libart_classes:
        raise ValueError("core-oj and core-libart DEX classes overlap")

    with tempfile.TemporaryDirectory(prefix="art-core-oj-") as temp:
        temp = Path(temp)
        oj_jar = temp / "core-oj-classes.jar"
        libart_jar = temp / "core-libart-classes.jar"
        output_dir = temp / "dex"
        output_dir.mkdir()
        extract_class_set(args.core_all, oj_classes, oj_jar)
        extract_class_set(args.core_all, libart_classes, libart_jar)

        subprocess.run(
            [
                args.java,
                "-cp",
                str(args.r8),
                "com.android.tools.r8.D8",
                "--release",
                "--min-api",
                "26",
                "--lib",
                str(libart_jar),
                "--output",
                str(output_dir),
                str(oj_jar),
            ],
            check=True,
        )
        dex_path = output_dir / "classes.dex"
        if not dex_path.is_file():
            raise ValueError("D8 did not create classes.dex")
        rewrite_hostdex(args.core_oj_hostdex, dex_path, oj_classes)

    print(
        f"Desugared core-oj: {len(oj_classes)} original classes preserved; "
        "invoke-custom support removed."
    )


if __name__ == "__main__":
    main()
