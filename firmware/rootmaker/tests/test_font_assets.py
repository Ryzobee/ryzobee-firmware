import hashlib
import json
import subprocess
import sys
import unittest
from io import BytesIO
from pathlib import Path

from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont


ROOT = Path(__file__).resolve().parents[1]
BUILD_SCRIPT = ROOT / "tools" / "build_font_assets.py"
MANIFEST = ROOT / "components" / "ryz_font" / "assets" / "manifest.json"


class FontAssetContractTest(unittest.TestCase):
    def test_teko_degree_preserves_ascii_and_matches_frozen_source(self) -> None:
        source = ROOT.parents[1] / "assets/typography/src/assets/fonts/teko-119.woff2"
        self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(),
                         "9f908ba2cfa6103dd08e4e59b6be48fafc790f55c7f718be712d3d13170a93fb")
        with TTFont(MANIFEST.parent / "teko_semibold_ascii.ttf") as embedded:
            self.assertIn(0xB0, embedded.getBestCmap())
            self.assertEqual(set(embedded.getBestCmap()), set(range(0x20, 0x7F)) | {0xB0})
            self.assertNotIn(0x2103, embedded.getBestCmap())

            def glyph_facts(font: TTFont, codepoint: int) -> list:
                name = font.getBestCmap()[codepoint]
                glyph = font["glyf"][name]
                coordinates, ends, flags = glyph.getCoordinates(font["glyf"])
                instructions = list(glyph.program.getBytecode()) if hasattr(glyph, "program") else []
                return [codepoint, list(coordinates), list(ends), list(flags),
                        instructions, list(font["hmtx"][name])]

            # Captured from the original 95-glyph asset before adding degree.
            # Includes every contour coordinate, flags, hint program and hmtx.
            ascii_facts = [glyph_facts(embedded, cp) for cp in range(0x20, 0x7F)]
            self.assertEqual(hashlib.sha256(json.dumps(
                ascii_facts, separators=(",", ":")).encode()).hexdigest(),
                "d39c638edca4422389860760a40fc64a37bdaaf5904e78eb3f97bd14c11fe42a")

            with TTFont(source, recalcTimestamp=False) as original:
                self.assertEqual([axis.axisTag for axis in original["fvar"].axes], ["wght"])
                self.assertIn(0xB0, original.getBestCmap())
                self.assertNotIn(0x2103, original.getBestCmap())
                frozen = instantiateVariableFont(original, {"wght": 600},
                    inplace=False, optimize=True, updateFontNames=True)
                try:
                    frozen.flavor = None
                    with BytesIO() as buffer:
                        # TTF serialization rounds fractional variable-font
                        # coordinates; compare that real static source shape.
                        frozen.save(buffer, reorderTables=False)
                        buffer.seek(0)
                        with TTFont(buffer) as expected:
                            self.assertNotIn("fvar", embedded)
                            self.assertEqual(embedded["OS/2"].usWeightClass, 600)
                            for cp in sorted(embedded.getBestCmap()):
                                self.assertEqual(glyph_facts(embedded, cp),
                                                 glyph_facts(expected, cp), hex(cp))
                finally:
                    frozen.close()

    def test_roboto_mono_official_source_and_license_are_frozen(self) -> None:
        component = MANIFEST.parent.parent
        source = component / "sources/roboto_mono/RobotoMono[wght].ttf"
        license_file = component / "LICENSES/RobotoMono-OFL.txt"
        self.assertEqual(source.stat().st_size, 183700)
        self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(),
                         "66a80e79d17e4c7cabd162e2916578a4cc08fd19eef6e2a643305eae9c567b2b")
        self.assertEqual(hashlib.sha256(license_file.read_bytes()).hexdigest(),
                         "50ab8dd54680d3473f649c9db86fece88434d097c7834475c1c72d2f8c429215")
        with TTFont(source) as font:
            self.assertEqual(font["name"].getDebugName(5), "Version 3.001")
            self.assertIn("SIL Open Font License, Version 1.1", font["name"].getDebugName(13))

    def test_embedded_ascii_fonts_match_their_sources(self) -> None:
        completed = subprocess.run(
            [sys.executable, str(BUILD_SCRIPT), "--check"],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(
            completed.returncode,
            0,
            msg=f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}",
        )

        manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
        self.assertEqual(manifest["schema"], "ryzobee-font-assets/v1")
        self.assertEqual(manifest["unicode_range"], "U+0020-007E")
        self.assertEqual(manifest["glyph_count"], 95)  # Common base, not each face's total.
        self.assertEqual(manifest["fonttools"], "4.60.2")
        self.assertEqual(
            {font["file"]: font["sha256"] for font in manifest["fonts"]},
            {
                "noto_sans_regular_ascii.ttf": "7f8e1c48bb4570229bccbf15e14206761366b01716a89f370c8c557f4983a880",
                "teko_semibold_ascii.ttf": "c67620f052fc43028871fab5b7b8409c0d1c5e2c38cec9ff1d4041fb4f19382a",
                "noto_sans_semibold_ascii.ttf": "5c0b2b801628f8bb77a0879d3dc32452b1660c7039096f25345c50bb0429b4b5",
                "noto_sans_medium_ascii.ttf": "e1acfef9f96cf7c52cbf593d40adcb50bb7244e706199f0076dd617e890ccd31",
                "roboto_mono_regular_ascii.ttf": "424b729fb28136675a1500e619572559eff4d66fd5f42754ead66f10fd74770e",
                "roboto_mono_medium_ascii.ttf": "5bfbfdc4b9226bfbbd08d693430390fa114939c14a8ddbb68298fa1873c35b3b",
                "roboto_mono_semibold_ascii.ttf": "b7f76f230ba7c114732e9ccf814579a109f2fd372228152c5e36b79fad0f52f9",
            },
        )
        self.assertEqual(
            {(font["role"], font["weight"]) for font in manifest["fonts"]},
            {("body", 400), ("display", 600), ("body_semibold", 600),
             ("body_medium", 500), ("mono", 400), ("mono_medium", 500),
             ("mono_semibold", 600)},
        )

        expected_names = {
            "noto_sans_regular_ascii.ttf": {
                1: "Noto Sans",
                2: "Regular",
                4: "Noto Sans Regular",
                6: "NotoSans-Regular",
            },
            "teko_semibold_ascii.ttf": {
                1: "Teko",
                2: "SemiBold",
                4: "Teko SemiBold",
                6: "Teko-SemiBold",
            },
            "noto_sans_semibold_ascii.ttf": {
                1: "Noto Sans",
                2: "SemiBold",
                4: "Noto Sans SemiBold",
                6: "NotoSans-SemiBold",
            },
            "noto_sans_medium_ascii.ttf": {
                1: "Noto Sans",
                2: "Medium",
                4: "Noto Sans Medium",
                6: "NotoSans-Medium",
            },
            "roboto_mono_regular_ascii.ttf": {
                1: "Roboto Mono",
                2: "Regular",
                4: "Roboto Mono Regular",
                6: "RobotoMono-Regular",
            },
            "roboto_mono_medium_ascii.ttf": {
                1: "Roboto Mono",
                2: "Medium",
                4: "Roboto Mono Medium",
                6: "RobotoMono-Medium",
            },
            "roboto_mono_semibold_ascii.ttf": {
                1: "Roboto Mono",
                2: "SemiBold",
                4: "Roboto Mono SemiBold",
                6: "RobotoMono-SemiBold",
            },
        }
        expected_weights = {
            "noto_sans_regular_ascii.ttf": 400,
            "teko_semibold_ascii.ttf": 600,
            "noto_sans_semibold_ascii.ttf": 600,
            "noto_sans_medium_ascii.ttf": 500,
            "roboto_mono_regular_ascii.ttf": 400,
            "roboto_mono_medium_ascii.ttf": 500,
            "roboto_mono_semibold_ascii.ttf": 600,
        }
        for file_name, names in expected_names.items():
            font = TTFont(MANIFEST.parent / file_name)
            expected_cmap = set(range(0x20, 0x7F))
            if file_name in {"teko_semibold_ascii.ttf", "noto_sans_semibold_ascii.ttf"}:
                expected_cmap.add(0xB0)
            if file_name in {"noto_sans_medium_ascii.ttf", "roboto_mono_medium_ascii.ttf"}:
                expected_cmap.add(0xB7)
            self.assertEqual(set(font.getBestCmap()), expected_cmap)
            self.assertEqual(set().union(*(set(table.cmap) for table in font["cmap"].tables
                                           if table.isUnicode())), expected_cmap)
            self.assertNotIn("fvar", font)
            self.assertEqual(font["OS/2"].usWeightClass, expected_weights[file_name])
            if file_name.startswith("roboto_mono"):
                self.assertEqual(len({font["hmtx"][glyph][0]
                                      for glyph in font.getBestCmap().values()}), 1)
            actual_names = {
                name_id: font["name"].getDebugName(name_id) for name_id in names
            }
            font.close()
            self.assertEqual(actual_names, names)

        source_hashes = {
            "assets/typography/src/assets/fonts/noto-sans-015.woff2":
                "51ca196f49a33e79e7870ff88ebd2829a3f627a51e7d690986618f0e7ad2b52d",
            "assets/typography/src/assets/fonts/teko-119.woff2":
                "9f908ba2cfa6103dd08e4e59b6be48fafc790f55c7f718be712d3d13170a93fb",
            "firmware/rootmaker/components/ryz_font/sources/roboto_mono/RobotoMono[wght].ttf":
                "66a80e79d17e4c7cabd162e2916578a4cc08fd19eef6e2a643305eae9c567b2b",
        }
        for font in manifest["fonts"]:
            expected_extra = {
                "display": ["U+00B0"],
                "body_semibold": ["U+00B0"],
                "body_medium": ["U+00B7"],
                "mono_medium": ["U+00B7"],
            }.get(font["role"], [])
            self.assertEqual(font.get("extra_codepoints", []), expected_extra)
            self.assertIn(font["source"], source_hashes)
            self.assertEqual(font["source_sha256"], source_hashes[font["source"]])
            source = ROOT.parents[1] / font["source"]
            self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(),
                             source_hashes[font["source"]])


if __name__ == "__main__":
    unittest.main()
