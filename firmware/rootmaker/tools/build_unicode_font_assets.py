#!/usr/bin/env python3
"""Decode vendored variable WOFF2 shards without changing their repertoire.

This is an offline artifact directory, not a firmware font-pack ABI. Completion
requires manifest.json and absence of .incomplete. Failed writes are retained
for inspection; this tool never cleans or overwrites a nonempty destination.
"""

from __future__ import annotations

import argparse
import hashlib
from io import BytesIO
import json
from pathlib import Path
import sys

from fontTools import __version__ as fonttools_version
from fontTools.ttLib import TTFont


ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parents[1]
WEB_ROOT = REPOSITORY / "assets/typography"
SOURCE_DIR = WEB_ROOT / "src/assets/fonts"
SOURCE_MANIFEST = SOURCE_DIR / "manifest.json"
SOURCE_CSS = WEB_ROOT / "src/fonts.css"
ASCII_DIR = ROOT / "components/ryz_font/assets"
REQUIRED_WEIGHTS = (400, 500, 600)
FAMILIES = {
    "Noto Sans": {
        "files": tuple(f"noto-sans-{index:03}.woff2" for index in range(8, 16)),
        "face_name": "Noto Sans",
        "default_weight": 400,
        "license": "OFL-NotoSans.txt",
    },
    "Noto Sans SC": {
        "files": tuple(f"noto-sans-sc-{index:03}.woff2" for index in range(16, 117)),
        "face_name": "Noto Sans SC Thin",
        "default_weight": 100,
        "license": "OFL-NotoSansSC.txt",
    },
}


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def source_record(path: Path, data: bytes) -> dict:
    return {"path": path.relative_to(REPOSITORY).as_posix(),
            "bytes": len(data), "sha256": digest(data)}


def coverage(codepoints: set[int]) -> dict:
    ranges = []
    values = sorted(codepoints)
    for value in values:
        if ranges and value == ranges[-1][1] + 1:
            ranges[-1][1] = value
        else:
            ranges.append([value, value])
    return {"codepoint_count": len(values), "ranges": [
        f"U+{first:04X}" if first == last else f"U+{first:04X}-{last:04X}"
        for first, last in ranges]}


def unicode_cmap(font: TTFont) -> set[int]:
    return {codepoint for table in font["cmap"].tables if table.isUnicode()
            for codepoint in getattr(table, "cmap", {})}


def axes(font: TTFont) -> list[dict]:
    return [{"tag": axis.axisTag, "min": axis.minValue,
             "default": axis.defaultValue, "max": axis.maxValue}
            for axis in font["fvar"].axes]


def checked_destination(output_dir: Path) -> Path:
    destination = output_dir.expanduser().absolute()
    # Reject aliases, including dangling symlinks, before resolve can hide them.
    if any(path.is_symlink() for path in (destination, *destination.parents)):
        raise ValueError("output path must not contain symlinks; use its real path")
    destination = destination.resolve()
    for protected in (SOURCE_DIR, WEB_ROOT / "public/fonts", ASCII_DIR):
        protected = protected.resolve()
        if destination.is_relative_to(protected) or protected.is_relative_to(destination):
            raise ValueError("output must not overlap vendored or existing ASCII font resources")
    if destination.exists() and (
        not destination.is_dir() or next(destination.iterdir(), None) is not None
    ):
        raise ValueError("output must be a nonexistent or empty directory")
    return destination


def decode_shard(row: dict, family: str) -> tuple[bytes, dict, set[int]]:
    filename = row["file"]
    source = SOURCE_DIR / filename
    raw = source.read_bytes()
    if (row.get("signature") != "wOF2" or raw[:4] != b"wOF2" or
        type(row.get("bytes")) is not int or len(raw) != row["bytes"] or
        digest(raw) != row.get("sha256")):
        raise ValueError(f"source manifest integrity mismatch: {filename}")
    with TTFont(BytesIO(raw), recalcTimestamp=False, recalcBBoxes=False) as font:
        required = {"fvar", "gvar", "glyf", "cmap", "name", "OS/2", "head"}
        if font.flavor != "woff2" or not required.issubset(font.keys()):
            raise ValueError(f"not a supported variable TrueType WOFF2 shard: {filename}")
        definition = FAMILIES[family]
        if (font["name"].getDebugName(1) != definition["face_name"] or
            font["OS/2"].fsSelection & 1 or font["head"].macStyle & 2):
            raise ValueError(f"font family or upright style mismatch: {filename}")
        source_axes = axes(font)
        expected_axes = [{"tag": "wght", "min": 100,
                          "default": definition["default_weight"], "max": 900}]
        if source_axes != expected_axes:
            raise ValueError(f"unexpected variable axes: {filename}")
        codepoints = unicode_cmap(font)
        if not codepoints:
            raise ValueError(f"empty Unicode cmap: {filename}")
        source_tables = set(font.keys())
        preserved = {tag: font.getTableData(tag) for tag in ("fvar", "gvar", "cmap")}
        glyph_order = font.getGlyphOrder()
        font.flavor = None
        output = BytesIO()
        font.save(output, reorderTables=False)
        decoded = output.getvalue()
    with TTFont(BytesIO(decoded), recalcTimestamp=False, recalcBBoxes=False) as result:
        if (result.flavor is not None or result.sfntVersion != "\x00\x01\x00\x00" or
            set(result.keys()) != source_tables or unicode_cmap(result) != codepoints or
            result.getGlyphOrder() != glyph_order or axes(result) != source_axes or
            any(result.getTableData(tag) != data for tag, data in preserved.items())):
            raise ValueError(f"raw TTF preservation check failed: {filename}")
    entry = {
        "family": family, "style": "normal", "source": source_record(source, raw),
        "file": Path(filename).with_suffix(".ttf").name,
        "bytes": len(decoded), "sha256": digest(decoded), "axes": source_axes,
        "coverage": coverage(codepoints), "tables": sorted(source_tables - {"GlyphOrder"}),
    }
    return decoded, entry, codepoints


def write_exclusive(path: Path, data: bytes) -> None:
    with path.open("xb") as stream:
        stream.write(data)


def build_into(output_dir: Path) -> dict:
    destination = checked_destination(output_dir)
    manifest_data = SOURCE_MANIFEST.read_bytes()
    source_manifest = json.loads(manifest_data)
    css_data = SOURCE_CSS.read_bytes()
    artifacts: list[tuple[str, bytes]] = []
    fonts, families, licenses = [], [], []
    combined: set[int] = set()
    for family, definition in FAMILIES.items():
        rows = [row for row in source_manifest["fonts"]
                if row.get("family") == family and row.get("style") == "normal"]
        if (sorted(row["file"] for row in rows) != list(definition["files"]) or
            any(row.get("weight") != "400 700" for row in rows)):
            raise ValueError(f"unexpected vendored shard inventory: {family}")
        family_points: set[int] = set()
        raw_bytes = source_bytes = 0
        for row in sorted(rows, key=lambda item: item["file"]):
            decoded, entry, points = decode_shard(row, family)
            artifacts.append((entry["file"], decoded))
            fonts.append(entry)
            family_points.update(points)
            raw_bytes += entry["bytes"]
            source_bytes += entry["source"]["bytes"]
        families.append({"family": family, "style": "normal", "shard_count": len(rows),
                         "source_bytes": source_bytes, "raw_bytes": raw_bytes,
                         "coverage": coverage(family_points)})
        combined.update(family_points)
        license_name = definition["license"]
        license_rows = [row for row in source_manifest["licenses"] if row.get("family") == family]
        if len(license_rows) != 1 or license_rows[0].get("file") != f"public/fonts/{license_name}":
            raise ValueError(f"unexpected license source: {family}")
        license_path = WEB_ROOT / "public/fonts" / license_name
        license_data = license_path.read_bytes()
        if b"SIL OPEN FONT LICENSE Version 1.1" not in license_data or b"Copyright" not in license_data:
            raise ValueError(f"missing full source font license: {family}")
        licenses.append({"family": family, "source": source_record(license_path, license_data),
                         "file": f"LICENSES/{license_name}",
                         "bytes": len(license_data), "sha256": digest(license_data)})
        artifacts.append((f"LICENSES/{license_name}", license_data))

    manifest = {
        "schema": "ryzobee-unicode-font-assets/v1", "fonttools": fonttools_version,
        "status": "storage-not-integrated", "format": "raw-variable-ttf-shards",
        "binary_pack_abi": False, "required_weights": list(REQUIRED_WEIGHTS),
        "coverage_basis": "union of Unicode cmap scalar mappings, not CSS ranges or complete Unicode",
        "transforms": {"freeze": False, "merge": False, "subset": False, "drop_tables": False},
        "source_manifest": source_record(SOURCE_MANIFEST, manifest_data),
        "source_css": source_record(SOURCE_CSS, css_data),
        "shard_count": len(fonts), "raw_bytes": sum(font["bytes"] for font in fonts),
        "coverage": coverage(combined), "families": families, "fonts": fonts,
        "licenses": licenses,
    }
    # Source decoding/validation finishes before any output is created. Recheck
    # emptiness, use exclusive file creation, and publish completion last.
    checked_destination(destination)
    destination.mkdir(parents=True, exist_ok=True)
    marker = destination / ".incomplete"
    write_exclusive(marker, b"Offline font export is incomplete. Do not integrate.\n")
    (destination / "LICENSES").mkdir()
    for filename, data in artifacts:
        write_exclusive(destination / filename, data)
    write_exclusive(destination / "manifest.json",
                    (json.dumps(manifest, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))
    marker.unlink()
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", required=True, type=Path,
                        help="explicit nonexistent/empty real directory; never existing font assets")
    args = parser.parse_args()
    try:
        manifest = build_into(args.output_dir)
    except Exception as error:
        print(f"UNICODE_FONT_ASSETS_FAILED: {error}; any partial output is not complete", file=sys.stderr)
        return 1
    print(f"UNICODE_FONT_ASSETS_OK shards={manifest['shard_count']} raw_bytes={manifest['raw_bytes']} "
          f"codepoints={manifest['coverage']['codepoint_count']} status=storage-not-integrated")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
