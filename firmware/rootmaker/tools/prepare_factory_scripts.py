#!/usr/bin/env python3
"""Stage factory Lua plus Store-compatible dates, without changing source fs/.

Dates describe creation of the files in this factory image, not script authorship.
SOURCE_DATE_EPOCH makes release packaging reproducible; otherwise use host UTC.
No runtime backfill, NTP dependency or filesystem layout migration is involved.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import time


def sha(data):
    return hashlib.sha256(data).hexdigest()


def time_record(name, source, epoch):
    # RYZTM01: char[8], [41], [65], [17], [17], [65]; no alignment padding.
    prefix = (b"RYZTM01\0" + name.encode("ascii").ljust(41, b"\0") +
              sha(source).encode("ascii") + b"\0" +
              (format(epoch, "016x").encode("ascii") + b"\0") * 2)
    record = prefix + sha(prefix).encode("ascii") + b"\0"
    assert len(record) == 213
    return record


def prepare(source, output, epoch):
    source, output = Path(source).absolute(), Path(output).absolute()
    if not 1704067200 <= epoch <= 253402300799:
        raise ValueError("Factory UTC must be within 2024-01-01 through 9999-12-31")
    if output.name != "factory-scripts" or output.is_symlink():
        raise ValueError("Output must be a non-symlink build/factory-scripts directory")
    if (source.resolve() == output.resolve() or source.resolve() in output.resolve().parents
            or output.resolve() in source.resolve().parents):
        raise ValueError("Factory source and generated output must not overlap")
    files = {}
    for path in sorted(source.iterdir()):
        if (path.is_symlink() or not path.is_file() or
                not re.fullmatch(r"[A-Za-z0-9_-]{1,36}\.lua", path.name)):
            raise ValueError("Unexpected factory source entry: " + path.name)
        data = path.read_bytes()
        if not data or len(data) > 16384 or b"\0" in data:
            raise ValueError("Invalid factory Lua size/content: " + path.name)
        files[path.name] = data
        files[".ryz-t" + sha(path.name.encode("ascii"))[:24]] = time_record(path.name, data, epoch)
    if not files:
        raise ValueError("Factory source is empty")

    # Manifest is outside the SPIFFS input. Only previously generated, unchanged
    # files may be replaced/removed; unknown files or symlinks fail closed.
    manifest = output.with_name("factory-scripts.manifest.json")
    if manifest.is_symlink():
        raise ValueError("Generated manifest must not be a symlink")
    previous = json.loads(manifest.read_text()) if manifest.exists() else {}
    if not isinstance(previous, dict):
        raise ValueError("Invalid generated-file manifest")
    actual = list(output.iterdir()) if output.exists() else []
    if {path.name for path in actual} != set(previous):
        raise ValueError("Unexpected files in generated factory directory; preserve and inspect")
    for path in actual:
        if path.is_symlink() or not path.is_file() or sha(path.read_bytes()) != previous[path.name]:
            raise ValueError("Generated factory file was modified: " + path.name)
    output.mkdir(parents=True, exist_ok=True)
    for path in actual:
        if path.name not in files:
            path.unlink()
    for name, data in files.items():
        (output / name).write_bytes(data)
    manifest.write_text(json.dumps({name: sha(data) for name, data in sorted(files.items())},
                                   indent=2) + "\n", encoding="utf-8")
    print("FACTORY_SCRIPTS_READY scripts=%d utc=%d time_records=%d" %
          (len(files) // 2, epoch, len(files) // 2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    prepare(args.source, args.output, int(os.environ.get("SOURCE_DATE_EPOCH", str(int(time.time())))))
