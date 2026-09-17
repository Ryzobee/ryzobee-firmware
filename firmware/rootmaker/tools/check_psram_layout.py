#!/usr/bin/env python3
"""Verify actual ESP32-S3 ELF placement, not just source attributes.

Run after idf.py build. No serial/device access and no third-party packages.
The 30 KiB floor covers the audited app snapshots plus IDF network/Host BSS;
it is a link-time budget, not a claim about runtime free heap or fragmentation.
"""
import argparse
import json
from pathlib import Path
import struct


EXTERNAL = {
    "ryz_monitor_stream.c": ("s_live", "s_frozen"),
    "workbench_scripts.c": ("published", "view", "last_model", "reader", "catalog"),
    "ryz_apps.c": ("s_snapshot",),
    "workbench_apps.c": ("published", "view"),
    "workbench.c": ("apps_ui_copy",),
    "ryz_v5_apps.c": ("model", "incoming", "list_page"),
}
INTERNAL = {
    "ryz_monitor_stream.c": ("s_lock", "s_accepting", "s_gap"),
    "workbench_scripts.c": ("gate", "initialized", "live_view"),
    "ryz_apps.c": ("s_lifecycle",),
    "ryz_apps_esp.c": ("s_lock", "s_reader"),
    "workbench_apps.c": ("gate", "initialized", "run_slot"),
    "ryz_v5_apps.c": ("binding", "press_token", "feedback"),
}


def read_layout(path):
    data = path.read_bytes()
    if data[:7] != b"\x7fELF\x01\x01\x01":
        raise ValueError("Expected little-endian ELF32 target image")
    header = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
    if header[2] != 94:  # EM_XTENSA; do not accept a host simulator artifact.
        raise ValueError("Expected Xtensa firmware, not host/simulator ELF")
    offset, entry_size, count, names_index = header[6], header[11], header[12], header[13]
    sections = [struct.unpack_from("<10I", data, offset + i * entry_size)
                for i in range(count)]

    def cstring(table, index):
        return table[index:table.index(b"\0", index)].decode("utf-8")

    def contents(section):
        return data[section[4]:section[4] + section[5]]

    names = contents(sections[names_index])
    section_names = [cstring(names, section[0]) for section in sections]
    sizes = {name: section[5] for name, section in zip(section_names, sections)}
    symbols = {}
    for section in sections:
        if section[1] != 2:  # SHT_SYMTAB
            continue
        strings = contents(sections[section[6]])
        source = ""
        for pos in range(section[4], section[4] + section[5], section[9]):
            name, address, size, info, _, index = struct.unpack_from("<IIIBBH", data, pos)
            name = cstring(strings, name)
            kind, binding = info & 15, info >> 4
            if kind == 4:  # STT_FILE scopes local symbols with duplicate names.
                source = name
            elif kind == 1 and binding == 0 and index < len(sections):
                symbols[source, name] = {
                    "address": hex(address), "bytes": size,
                    "section": section_names[index],
                }
    return sizes, symbols


def check(path):
    sizes, symbols = read_layout(path)
    errors, migrated = [], {}
    for expected, files in ((True, EXTERNAL), (False, INTERNAL)):
        for source, names in files.items():
            for name in names:
                label = source + ":" + name
                symbol = symbols.get((source, name))
                if symbol is None:
                    errors.append(label + " missing; audit symbol changes explicitly")
                    continue
                required = (".ext_ram.bss",) if expected else (".dram0.bss", ".dram0.data")
                if symbol["section"] not in required:
                    errors.append(label + " in " + symbol["section"] + "; expected " + str(required))
                if expected:
                    migrated[label] = symbol
    external_bytes = sizes.get(".ext_ram.bss", 0)
    if external_bytes < 30 * 1024:
        errors.append("External BSS below audited 30 KiB link-time budget")
    report = {
        "elf": str(path.resolve()), "passed": not errors,
        "internal_bss_bytes": sizes.get(".dram0.bss", 0),
        "internal_data_bytes": sizes.get(".dram0.data", 0),
        "external_bss_bytes": external_bytes,
        "audited_application_bytes": sum(s["bytes"] for s in migrated.values()),
        "audited_symbols": migrated, "errors": errors,
    }
    print(json.dumps(report, indent=2))
    return 0 if not errors else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", type=Path,
                        default=Path(__file__).resolve().parents[1] / "build/ryzobee_rootmaker.elf")
    raise SystemExit(check(parser.parse_args().elf))
