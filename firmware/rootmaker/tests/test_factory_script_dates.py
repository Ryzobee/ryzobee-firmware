"""Packaging boundaries; real C Store consumes these bytes in test_script_store_time."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/prepare_factory_scripts.py"
SPEC = importlib.util.spec_from_file_location("factory_dates", TOOL)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
EPOCH = 1735689600


class FactoryDatesTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="ryz-factory-dates-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.source = self.root / "fs"
        self.source.mkdir()
        self.output = self.root / "factory-scripts"
        (self.source / "a.lua").write_bytes(b"print('a')\n")

    def prepare(self, epoch=EPOCH):
        MODULE.prepare(self.source, self.output, epoch)

    def test_source_unchanged_exact_layout_and_deterministic_repack(self):
        original = (self.source / "a.lua").read_bytes()
        self.prepare()
        first = {path.name: path.read_bytes() for path in self.output.iterdir()}
        self.prepare()
        self.assertEqual(first, {path.name: path.read_bytes() for path in self.output.iterdir()})
        self.assertEqual((self.source / "a.lua").read_bytes(), original)
        self.assertEqual(list(self.source.iterdir()), [self.source / "a.lua"])
        name = ".ryz-t" + hashlib.sha256(b"a.lua").hexdigest()[:24]
        record = first[name]
        self.assertEqual(len(record), 213)
        self.assertEqual(record[:8], b"RYZTM01\0")
        self.assertEqual(record[8:49], b"a.lua".ljust(41, b"\0"))
        self.assertEqual(record[49:114], hashlib.sha256(original).hexdigest().encode() + b"\0")
        self.assertEqual(int(record[114:130], 16), EPOCH)
        self.assertEqual(int(record[131:147], 16), EPOCH)
        self.assertEqual(record[148:], hashlib.sha256(record[:148]).hexdigest().encode() + b"\0")

    def test_removed_factory_file_and_its_sidecar_do_not_reappear(self):
        (self.source / "b.lua").write_bytes(b"print('b')\n")
        self.prepare()
        (self.source / "b.lua").unlink()
        self.prepare()
        self.assertEqual(len(list(self.output.iterdir())), 2)
        self.assertFalse((self.output / "b.lua").exists())

    def test_changed_source_gets_matching_hash_and_new_image_dates(self):
        self.prepare()
        (self.source / "a.lua").write_bytes(b"print('changed')\n")
        self.prepare(EPOCH + 86400)
        record = next(self.output.glob(".ryz-t*")).read_bytes()
        self.assertEqual(record[49:113].decode(), hashlib.sha256(b"print('changed')\n").hexdigest())
        self.assertEqual(int(record[114:130], 16), EPOCH + 86400)
        self.assertEqual(int(record[131:147], 16), EPOCH + 86400)

    def test_unknown_and_modified_generated_files_are_preserved(self):
        self.prepare()
        unknown = self.output / "user.txt"
        unknown.write_text("preserve")
        with self.assertRaisesRegex(ValueError, "Unexpected files"):
            self.prepare()
        self.assertEqual(unknown.read_text(), "preserve")
        unknown.unlink()
        (self.output / "a.lua").write_text("user change")
        with self.assertRaisesRegex(ValueError, "was modified"):
            self.prepare()
        self.assertEqual((self.output / "a.lua").read_text(), "user change")

    def test_symlinks_and_source_output_overlap_are_rejected(self):
        (self.source / "alias.lua").symlink_to(self.source / "a.lua")
        with self.assertRaisesRegex(ValueError, "Unexpected factory source"):
            self.prepare()
        (self.source / "alias.lua").unlink()
        self.output.symlink_to(self.source, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "non-symlink"):
            self.prepare()
        self.output.unlink()
        with self.assertRaisesRegex(ValueError, "must not overlap"):
            MODULE.prepare(self.source, self.source / "factory-scripts", EPOCH)

    def test_invalid_epoch_or_input_cannot_overwrite_good_output(self):
        self.prepare()
        manifest = self.root / "factory-scripts.manifest.json"
        before = manifest.read_bytes()
        for epoch in (0, 1704067199, 253402300800):
            with self.subTest(epoch=epoch), self.assertRaises(ValueError):
                self.prepare(epoch)
        for source in (b"", b"\0", b"x" * 16385):
            (self.source / "a.lua").write_bytes(source)
            with self.subTest(source_length=len(source)), self.assertRaises(ValueError):
                self.prepare()
        self.assertEqual(manifest.read_bytes(), before)

    def test_cli_uses_source_date_epoch_and_manifest_is_not_in_spiffs_input(self):
        subprocess.run([sys.executable, str(TOOL), "--source", str(self.source),
                        "--output", str(self.output)], check=True, capture_output=True,
                       env={**os.environ, "SOURCE_DATE_EPOCH": str(EPOCH)}, timeout=10)
        self.assertEqual(len(list(self.output.iterdir())), 2)
        self.assertEqual(len(json.loads((self.root / "factory-scripts.manifest.json").read_text())), 2)


if __name__ == "__main__":
    unittest.main()
