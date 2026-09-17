"""Offline font feasibility; none of these assets are firmware inputs yet."""

import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fontTools.ttLib import TTFont


ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parents[1]
BUILDER = ROOT / "tools/build_unicode_font_assets.py"
WEB_FONTS = REPOSITORY / "assets/typography/src/assets/fonts"
EXPECTED_COUNTS = {"Noto Sans": 2370, "Noto Sans SC": 13635}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def coverage_ranges(codepoints):
    """Independent canonical inclusive range serialization."""
    ranges = []
    for codepoint in sorted(codepoints):
        if ranges and ranges[-1][1] + 1 == codepoint:
            ranges[-1][1] = codepoint
        else:
            ranges.append([codepoint, codepoint])
    return [f"U+{first:04X}" if first == last else f"U+{first:04X}-{last:04X}"
            for first, last in ranges]


class UnicodeFontAssetTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="ryz-unicode-assets-test-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name).resolve()
        cls.output = cls.directory / "first"
        completed = subprocess.run(
            [sys.executable, str(BUILDER), "--output-dir", str(cls.output)],
            capture_output=True, text=True, timeout=120, check=False,
        )
        if completed.returncode != 0:
            raise AssertionError(completed.stdout + completed.stderr)
        cls.manifest = json.loads((cls.output / "manifest.json").read_text())
        spec = importlib.util.spec_from_file_location("unicode_assets_under_test", BUILDER)
        cls.builder = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.builder)

    def test_all_source_cmaps_axes_and_tables_survive_decoding(self):
        manifest = self.manifest
        self.assertEqual(manifest["schema"], "ryzobee-unicode-font-assets/v1")
        self.assertEqual(manifest["status"], "storage-not-integrated")
        self.assertEqual(manifest["required_weights"], [400, 500, 600])
        self.assertFalse((self.output / ".incomplete").exists())
        source_manifest = json.loads((WEB_FONTS / "manifest.json").read_text())
        source_records = {item["file"]: item for item in source_manifest["fonts"]
                          if item["family"] in EXPECTED_COUNTS
                          and item["style"] == "normal"}
        self.assertEqual(len(source_records), 109)
        self.assertEqual(len(manifest["fonts"]), 109)
        self.assertEqual({Path(item["source"]["path"]).name
                          for item in manifest["fonts"]}, set(source_records))
        codepoints = {family: set() for family in EXPECTED_COUNTS}
        raw_bytes = 0
        for item in manifest["fonts"]:
            source = REPOSITORY / item["source"]["path"]
            output = self.output / item["file"]
            original_record = source_records[source.name]
            with self.subTest(source=source.name):
                self.assertEqual(item["family"], original_record["family"])
                self.assertEqual(item["style"], "normal")
                self.assertEqual(item["source"]["sha256"], sha256(source))
                self.assertEqual(item["source"]["sha256"], original_record["sha256"])
                self.assertEqual(item["source"]["bytes"], source.stat().st_size)
                self.assertEqual(item["sha256"], sha256(output))
                self.assertEqual(item["bytes"], output.stat().st_size)
                with TTFont(source) as before, TTFont(output) as after:
                    self.assertEqual(before.flavor, "woff2")
                    self.assertIsNone(after.flavor)
                    self.assertEqual(set(before.keys()), set(after.keys()))
                    self.assertEqual(before.getBestCmap(), after.getBestCmap())
                    def unicode_cmaps(font):
                        return [(table.platformID, table.platEncID, table.format,
                                 table.language, sorted(table.cmap.items()))
                                for table in font["cmap"].tables if table.isUnicode()]
                    self.assertEqual(unicode_cmaps(before), unicode_cmaps(after))
                    before_axes = [(axis.axisTag, axis.minValue, axis.defaultValue,
                                    axis.maxValue) for axis in before["fvar"].axes]
                    after_axes = [(axis.axisTag, axis.minValue, axis.defaultValue,
                                   axis.maxValue) for axis in after["fvar"].axes]
                    self.assertEqual(before_axes, after_axes)
                    self.assertIn("gvar", after)
                    points = set(after.getBestCmap())
                    self.assertEqual(item["coverage"]["codepoint_count"], len(points))
                    self.assertEqual(item["coverage"]["ranges"], coverage_ranges(points))
                    codepoints[item["family"]].update(points)
            raw_bytes += output.stat().st_size
        self.assertEqual({family: len(points) for family, points in codepoints.items()},
                         EXPECTED_COUNTS)
        union = set.union(*codepoints.values())
        self.assertEqual(len(union), 15568)
        self.assertEqual(manifest["coverage"]["codepoint_count"], len(union))
        self.assertEqual(manifest["coverage"]["ranges"], coverage_ranges(union))
        self.assertEqual(manifest["raw_bytes"], raw_bytes)
        self.assertGreater(raw_bytes, 8 * 1024 * 1024)
        # This is evidence of current gaps, not a request to invent glyphs.
        self.assertTrue(set(map(ord, "中文网络，。！？")) <= codepoints["Noto Sans SC"])
        self.assertTrue(set(map(ord, "ŠšŽžŁłİıĞğ")) <= codepoints["Noto Sans"])
        self.assertTrue(set(map(ord, "𠮷𠀀😀")).isdisjoint(union))

    def test_generation_is_byte_reproducible(self):
        other = self.directory / "second"
        other.mkdir()  # Explicitly empty directories are also supported.
        completed = subprocess.run(
            [sys.executable, str(BUILDER), "--output-dir", str(other)],
            capture_output=True, text=True, timeout=120, check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        first_files = {path.relative_to(self.output): sha256(path)
                       for path in self.output.rglob("*") if path.is_file()}
        second_files = {path.relative_to(other): sha256(path)
                        for path in other.rglob("*") if path.is_file()}
        self.assertEqual(first_files, second_files)

    def test_complete_license_texts_are_copied(self):
        for family in ["NotoSans", "NotoSansSC"]:
            filename = f"OFL-{family}.txt"
            self.assertEqual((self.output / "LICENSES" / filename).read_bytes(),
                             (REPOSITORY / "assets/typography/public/fonts" / filename).read_bytes())

    def test_existing_output_is_never_overwritten(self):
        protected = self.directory / "protected"
        protected.mkdir()
        sentinel = protected / "sentinel.txt"
        # Test fixtures only; this is not a repository edit.
        sentinel.write_text("keep this existing file")
        original_files = {path.relative_to(self.output): sha256(path)
                          for path in self.output.rglob("*") if path.is_file()}
        for target in [protected, sentinel, self.output]:
            completed = subprocess.run(
                [sys.executable, str(BUILDER), "--output-dir", str(target)],
                capture_output=True, text=True, timeout=30, check=False,
            )
            self.assertNotEqual(completed.returncode, 0)
        self.assertEqual(list(protected.iterdir()), [sentinel])
        self.assertEqual(sentinel.read_text(), "keep this existing file")
        self.assertEqual(original_files,
                         {path.relative_to(self.output): sha256(path)
                          for path in self.output.rglob("*") if path.is_file()})

    def test_symlink_and_unspecified_output_are_rejected(self):
        empty = self.directory / "empty"
        empty.mkdir()
        for name, destination in [("linked", empty),
                                  ("dangling", self.directory / "missing")]:
            target = self.directory / name
            target.symlink_to(destination, target_is_directory=True)
            completed = subprocess.run(
                [sys.executable, str(BUILDER), "--output-dir", str(target)],
                capture_output=True, text=True, timeout=30, check=False,
            )
            self.assertNotEqual(completed.returncode, 0)
            self.assertTrue(target.is_symlink())
        self.assertEqual(list(empty.iterdir()), [])
        parent_alias = self.directory / "aliased-parent"
        parent_alias.symlink_to(empty, target_is_directory=True)
        completed = subprocess.run(
            [sys.executable, str(BUILDER), "--output-dir", str(parent_alias / "child")],
            capture_output=True, text=True, timeout=30, check=False,
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertEqual(list(empty.iterdir()), [])
        completed = subprocess.run([sys.executable, str(BUILDER)], capture_output=True,
                                   text=True, timeout=30, check=False)
        self.assertNotEqual(completed.returncode, 0)

    def test_source_hash_mismatch_fails_before_output_creation(self):
        corrupted = self.directory / "corrupted-sources"
        corrupted.mkdir()
        original = WEB_FONTS / "noto-sans-008.woff2"
        changed = bytearray(original.read_bytes())
        changed[-1] ^= 1
        (corrupted / original.name).write_bytes(changed)
        output = self.directory / "rejected-integrity"
        with mock.patch.object(self.builder, "SOURCE_DIR", corrupted):
            with self.assertRaisesRegex(ValueError, "integrity mismatch"):
                self.builder.build_into(output)
        self.assertFalse(output.exists())

    def test_partial_output_is_explicitly_incomplete_and_never_reused(self):
        output = self.directory / "interrupted"
        actual_write = self.builder.write_exclusive

        def fail_font_write(path, data):
            if path.suffix == ".ttf":
                raise OSError("injected write failure")
            actual_write(path, data)

        with mock.patch.object(self.builder, "write_exclusive", fail_font_write):
            with self.assertRaisesRegex(OSError, "injected write failure"):
                self.builder.build_into(output)
        self.assertTrue((output / ".incomplete").is_file())
        self.assertFalse((output / "manifest.json").exists())
        with self.assertRaisesRegex(ValueError, "nonexistent or empty"):
            self.builder.build_into(output)

    def test_managed_freetype_rasterizes_every_shard_at_all_three_weights(self):
        executable = os.environ.get("RYZ_UNICODE_FONT_PROBE")
        if not executable:
            self.skipTest("Set RYZ_UNICODE_FONT_PROBE to the managed-FreeType Host executable")
        family_checksums = {family: {weight: hashlib.sha256() for weight in [400, 500, 600]}
                            for family in EXPECTED_COUNTS}
        render_count = max_bitmap_bytes = 0
        for item in self.manifest["fonts"]:
            with self.subTest(font=item["file"]):
                completed = subprocess.run(
                    [executable, str(self.output / item["file"])],
                    capture_output=True, text=True, timeout=120, check=False,
                )
                self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
                result = json.loads(completed.stdout)
                max_bitmap_bytes = max(max_bitmap_bytes, result["max_bitmap_bytes"])
                self.assertEqual(result["codepoint_count"], item["coverage"]["codepoint_count"])
                self.assertEqual(result["weights"], [400, 500, 600])
                self.assertEqual(result["sizes"], [8, 12, 16])
                self.assertEqual(len(result["runs"]), 9)
                self.assertEqual({(run["weight"], run["size_px"])
                                  for run in result["runs"]},
                                 {(weight, size) for weight in [400, 500, 600]
                                  for size in [8, 12, 16]})
                for run in result["runs"]:
                    self.assertEqual(run["codepoint_count"],
                                     item["coverage"]["codepoint_count"])
                    self.assertEqual(len(run["checksum"]), 16)
                    family_checksums[item["family"]][run["weight"]].update(
                        run["checksum"].encode("ascii"))
                    render_count += run["codepoint_count"]
        # The native per-run hash excludes the requested weight. This checks
        # real raster/metric changes, not three different JSON labels. Hinted
        # individual glyphs may legitimately match at adjacent weights.
        for family, checksums in family_checksums.items():
            with self.subTest(family=family):
                self.assertEqual(len({value.hexdigest() for value in checksums.values()}), 3)
        print(f"UNICODE_FONT_PROBE_PASS shards={len(self.manifest['fonts'])} "
              f"renders={render_count} max_a8_bitmap_bytes={max_bitmap_bytes}", flush=True)

    def test_native_probe_rejects_invalid_and_nonvariable_input(self):
        executable = os.environ.get("RYZ_UNICODE_FONT_PROBE")
        if not executable:
            self.skipTest("Set RYZ_UNICODE_FONT_PROBE to test the native input guard")
        malformed = self.directory / "malformed.ttf"
        malformed.write_bytes(b"\x00\x01\x00\x00invalid")
        candidates = [[], [str(self.directory / "missing.ttf")], [str(malformed)],
                      [str(WEB_FONTS / "noto-sans-015.woff2")],
                      [str(ROOT / "components/ryz_font/assets/noto_sans_regular_ascii.ttf")]]
        for arguments in candidates:
            with self.subTest(arguments=arguments):
                completed = subprocess.run([executable, *arguments], capture_output=True,
                                           text=True, timeout=30, check=False)
                self.assertEqual(completed.returncode, 1, completed.stderr)
                self.assertEqual(completed.stdout, "")
                self.assertIn("unicode_font_probe", completed.stderr)


if __name__ == "__main__":
    unittest.main()
