#!/usr/bin/env python3
"""Build deterministic ASCII-base TTFs with explicitly listed per-face extras."""

from __future__ import annotations

import argparse
import hashlib
import json
import tempfile
from dataclasses import dataclass
from pathlib import Path

from fontTools import __version__ as fonttools_version
from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont


ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parents[1]
ASSET_DIR = ROOT / "components" / "ryz_font" / "assets"
PRINTABLE_ASCII = frozenset(range(0x20, 0x7F))


@dataclass(frozen=True)
class FontDefinition:
    role: str
    family: str
    style: str
    full_name: str
    postscript_name: str
    weight: int
    source: Path
    output_name: str
    extra_codepoints: tuple[int, ...] = ()


FONTS = (
    FontDefinition(
        role="body",
        family="Noto Sans",
        style="Regular",
        full_name="Noto Sans Regular",
        postscript_name="NotoSans-Regular",
        weight=400,
        source=REPOSITORY
        / "assets/typography/src/assets/fonts/noto-sans-015.woff2",
        output_name="noto_sans_regular_ascii.ttf",
    ),
    FontDefinition(
        role="display",
        family="Teko",
        style="SemiBold",
        full_name="Teko SemiBold",
        postscript_name="Teko-SemiBold",
        weight=600,
        source=REPOSITORY / "assets/typography/src/assets/fonts/teko-119.woff2",
        output_name="teko_semibold_ascii.ttf",
        extra_codepoints=(0x00B0,),
    ),
    FontDefinition(
        role="body_semibold",
        family="Noto Sans",
        style="SemiBold",
        full_name="Noto Sans SemiBold",
        postscript_name="NotoSans-SemiBold",
        weight=600,
        source=REPOSITORY / "assets/typography/src/assets/fonts/noto-sans-015.woff2",
        output_name="noto_sans_semibold_ascii.ttf",
        extra_codepoints=(0x00B0,),
    ),
    FontDefinition(
        role="body_medium",
        family="Noto Sans",
        style="Medium",
        full_name="Noto Sans Medium",
        postscript_name="NotoSans-Medium",
        weight=500,
        source=REPOSITORY / "assets/typography/src/assets/fonts/noto-sans-015.woff2",
        output_name="noto_sans_medium_ascii.ttf",
        extra_codepoints=(0x00B7,),
    ),
    FontDefinition(
        role="mono",
        family="Roboto Mono",
        style="Regular",
        full_name="Roboto Mono Regular",
        postscript_name="RobotoMono-Regular",
        weight=400,
        source=ROOT / "components/ryz_font/sources/roboto_mono/RobotoMono[wght].ttf",
        output_name="roboto_mono_regular_ascii.ttf",
    ),
    FontDefinition(
        role="mono_medium",
        family="Roboto Mono",
        style="Medium",
        full_name="Roboto Mono Medium",
        postscript_name="RobotoMono-Medium",
        weight=500,
        source=ROOT / "components/ryz_font/sources/roboto_mono/RobotoMono[wght].ttf",
        output_name="roboto_mono_medium_ascii.ttf",
        extra_codepoints=(0x00B7,),
    ),
    FontDefinition(
        role="mono_semibold",
        family="Roboto Mono",
        style="SemiBold",
        full_name="Roboto Mono SemiBold",
        postscript_name="RobotoMono-SemiBold",
        weight=600,
        source=ROOT / "components/ryz_font/sources/roboto_mono/RobotoMono[wght].ttf",
        output_name="roboto_mono_semibold_ascii.ttf",
    ),
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(64 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def freeze_weight(font: TTFont, weight: int) -> TTFont:
    if "fvar" not in font:
        return font
    location = {axis.axisTag: axis.defaultValue for axis in font["fvar"].axes}
    if "wght" not in location:
        raise RuntimeError("variable font has no wght axis")
    location["wght"] = weight
    return instantiateVariableFont(
        font, location, inplace=False, optimize=True, updateFontNames=True
    )


def normalize_names(font: TTFont, definition: FontDefinition) -> None:
    names = {
        1: definition.family,
        2: definition.style,
        4: definition.full_name,
        6: definition.postscript_name,
        16: definition.family,
        17: definition.style,
    }
    table = font["name"]
    for name_id, value in names.items():
        table.removeNames(nameID=name_id)
        table.setName(value, name_id, 3, 1, 0x409)
        table.setName(value, name_id, 1, 0, 0)


def build_font(definition: FontDefinition, output: Path) -> dict[str, object]:
    if not definition.source.is_file():
        raise FileNotFoundError(f"missing vendored font: {definition.source}")

    source_font = TTFont(definition.source, recalcTimestamp=False)
    font = freeze_weight(source_font, definition.weight)
    source_font.close()
    font.flavor = None
    normalize_names(font, definition)

    options = subset.Options()
    options.hinting = True
    options.layout_features = []
    options.name_IDs = [0, 1, 2, 3, 4, 5, 6, 13, 14]
    options.name_legacy = True
    options.name_languages = [0x409]
    options.recalc_timestamp = False
    options.drop_tables += ["DSIG"]
    subsetter = subset.Subsetter(options=options)
    coverage = PRINTABLE_ASCII | frozenset(definition.extra_codepoints)
    subsetter.populate(unicodes=coverage)
    subsetter.subset(font)

    output.parent.mkdir(parents=True, exist_ok=True)
    font.save(output, reorderTables=False)
    font.close()

    result = TTFont(output, recalcTimestamp=False)
    cmap = set(result.getBestCmap() or {})
    if cmap != coverage:
        missing = sorted(coverage - cmap)
        unexpected = sorted(cmap - coverage)
        raise RuntimeError(
            f"{output.name}: invalid cmap, missing={missing}, unexpected={unexpected}"
        )
    if "fvar" in result:
        raise RuntimeError(f"{output.name}: weight axis was not frozen")
    if result.flavor is not None:
        raise RuntimeError(f"{output.name}: expected raw TTF/OTF, got {result.flavor}")
    result.close()

    entry = {
        "role": definition.role,
        "family": definition.family,
        "style": definition.style,
        "weight": definition.weight,
        "source": str(definition.source.relative_to(REPOSITORY)),
        "source_sha256": sha256(definition.source),
        "file": definition.output_name,
        "bytes": output.stat().st_size,
        "sha256": sha256(output),
    }
    if definition.extra_codepoints:
        entry["extra_codepoints"] = [f"U+{cp:04X}" for cp in sorted(definition.extra_codepoints)]
    return entry


def build_into(output_dir: Path) -> dict[str, object]:
    fonts = [
        build_font(definition, output_dir / definition.output_name)
        for definition in FONTS
    ]
    return {
        "schema": "ryzobee-font-assets/v1",
        # v1 records the common base; each face explicitly lists any extras.
        "unicode_range": "U+0020-007E",
        "glyph_count": len(PRINTABLE_ASCII),
        "fonttools": fonttools_version,
        "fonts": fonts,
    }


def manifest_bytes(manifest: dict[str, object]) -> bytes:
    return (json.dumps(manifest, indent=2, ensure_ascii=False) + "\n").encode()


def check_assets() -> None:
    with tempfile.TemporaryDirectory(prefix="ryzobee-font-check-") as temporary:
        generated_dir = Path(temporary)
        manifest = build_into(generated_dir)
        expected_manifest = manifest_bytes(manifest)
        checked_manifest = ASSET_DIR / "manifest.json"
        if not checked_manifest.is_file():
            raise RuntimeError(f"missing generated manifest: {checked_manifest}")
        if checked_manifest.read_bytes() != expected_manifest:
            raise RuntimeError(
                "font asset manifest is stale; run tools/build_font_assets.py"
            )
        for definition in FONTS:
            checked = ASSET_DIR / definition.output_name
            generated = generated_dir / definition.output_name
            if not checked.is_file() or checked.read_bytes() != generated.read_bytes():
                raise RuntimeError(
                    f"font asset is stale: {checked}; run tools/build_font_assets.py"
                )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--check",
        action="store_true",
        help="regenerate in a temporary directory and compare with checked-in assets",
    )
    arguments = parser.parse_args()
    if arguments.check:
        check_assets()
        print("RYZOBEE_FONT_ASSETS_OK")
        return 0

    manifest = build_into(ASSET_DIR)
    (ASSET_DIR / "manifest.json").write_bytes(manifest_bytes(manifest))
    for font in manifest["fonts"]:
        print(
            f"{font['file']}: {font['bytes']} bytes sha256={font['sha256']}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
