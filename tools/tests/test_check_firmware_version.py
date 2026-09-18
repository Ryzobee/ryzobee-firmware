"""Exercise the public version-gate CLI in isolated, real Git repositories."""

import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import unittest


CHECKER = Path(__file__).resolve().parents[1] / "check_firmware_version.py"
FIRMWARE = Path("firmware/rootmaker")


class FirmwareVersionGateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ryz-version-gate-")
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name)
        self.env = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
        self.git("init", "--quiet")
        self.git("config", "user.name", "Version Gate Test")
        self.git("config", "user.email", "version-gate@example.invalid")
        (self.repo / FIRMWARE).mkdir(parents=True)
        self.write_version("0.10.2")
        self.git("add", ".")
        self.git("-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "fixture base")
        self.base = self.git("rev-parse", "HEAD").stdout.strip()

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.repo), *args], env=self.env,
                              text=True, capture_output=True, check=True)

    def write_version(self, version):
        (self.repo / FIRMWARE / "CMakeLists.txt").write_text(
            f'cmake_minimum_required(VERSION 3.16)\nset(PROJECT_VER "{version}")\n'
            'include($ENV{IDF_PATH}/tools/cmake/project.cmake)\nproject(ryzobee_rootmaker)\n',
            encoding="utf-8")

    def check(self, *args, expected=0):
        result = subprocess.run(
            [sys.executable, str(CHECKER), "--base-ref", self.base, *args],
            cwd=self.repo, env=self.env, text=True, capture_output=True)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        return result

    def replace_json(self, path, **values):
        contents = json.loads(path.read_text(encoding="utf-8"))
        contents.update(values)
        path.write_text(json.dumps(contents), encoding="utf-8")

    def test_patch_increment_is_accepted(self):
        self.write_version("0.10.3")
        result = self.check()
        self.assertIn("0.10.2 -> 0.10.3", result.stdout)

    def test_duplicate_noncanonical_assignment_is_rejected(self):
        self.write_version("0.10.3")
        path = self.repo / FIRMWARE / "CMakeLists.txt"
        with path.open("a", encoding="utf-8") as output:
            output.write('SET ( PROJECT_VER "0.10.2" )\n')
        self.assertIn("exactly one", self.check(expected=1).stderr)

    def test_comment_cannot_supply_the_version(self):
        path = self.repo / FIRMWARE / "CMakeLists.txt"
        path.write_text('#[=[\nset(PROJECT_VER "0.10.3")\n]=]\n', encoding="utf-8")
        self.assertIn("canonical", self.check(expected=1).stderr)

    def test_unchanged_version_fails_including_documentation_only_changes(self):
        (self.repo / "README.md").write_text("Documentation change only\n", encoding="utf-8")
        self.assertIn("must increase", self.check(expected=1).stderr)

    def test_regressions_are_rejected_by_numeric_order(self):
        for version in ("0.10.1", "0.9.99", "0.2.100"):
            with self.subTest(version=version):
                self.write_version(version)
                self.assertIn("must increase", self.check(expected=1).stderr)

    def test_minor_major_and_multiple_digit_increments_are_numeric(self):
        for version in ("0.10.10", "0.11.0", "1.0.0"):
            with self.subTest(version=version):
                self.write_version(version)
                self.assertIn(f"0.10.2 -> {version}", self.check().stdout)

    def test_decimal_carry_and_minor_increment_are_not_lexicographic(self):
        for base, current in (("0.10.9", "0.10.10"), ("0.9.99", "0.10.0")):
            with self.subTest(base=base, current=current):
                self.write_version(base)
                self.git("add", ".")
                self.git("-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "numeric base")
                self.base = self.git("rev-parse", "HEAD").stdout.strip()
                self.write_version(current)
                self.assertIn(f"{base} -> {current}", self.check().stdout)

    def test_noncanonical_semver_is_rejected(self):
        for version in ("V0.10.3", "0.10", "0.10.3.0", "0.10.3-beta", "0.10.3+build",
                        "00.10.3", "0.010.3", "0.10.03", "-1.10.3", "0.10.３", "", "0.10.3 "):
            with self.subTest(version=version):
                self.write_version(version)
                self.assertIn("canonical", self.check(expected=1).stderr)

    def test_version_cannot_be_truncated_by_idf(self):
        self.write_version("123456789012345678901234567890.0.0")
        self.assertIn("31-byte", self.check(expected=1).stderr)

    def test_missing_or_duplicate_declarations_are_rejected(self):
        path = self.repo / FIRMWARE / "CMakeLists.txt"
        for source in ("project(example)\n", 'set(PROJECT_VER "0.10.3")\n' * 2,
                       'set(PROJECT_VER "0.10.3" CACHE STRING "version")\n',
                       'set(PROJECT_VER\n "0.10.3")\n',
                       '#[[ unfinished comment\nset(PROJECT_VER "0.10.3")\n'):
            with self.subTest(source=source):
                path.write_text(source, encoding="utf-8")
                self.assertIn("canonical", self.check(expected=1).stderr)

    def test_comment_examples_are_ignored(self):
        self.write_version("0.10.3")
        with (self.repo / FIRMWARE / "CMakeLists.txt").open("a", encoding="utf-8") as output:
            output.write('# set(PROJECT_VER "9.0.0")\n#[[\nset(PROJECT_VER "2.0.0")\n]]\n')
        self.check()

    def test_invalid_base_declaration_is_not_silently_treated_as_zero(self):
        for source in ('set(PROJECT_VER "dev")\n', "project(example)\n",
                       'set(PROJECT_VER "0.10.2")\n' * 2):
            with self.subTest(source=source):
                (self.repo / FIRMWARE / "CMakeLists.txt").write_text(source, encoding="utf-8")
                self.git("add", ".")
                self.git("-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "bad base")
                self.base = self.git("rev-parse", "HEAD").stdout.strip()
                self.write_version("0.10.3")
                self.assertIn("base:", self.check(expected=1).stderr)

    def test_missing_base_ref_fails_closed(self):
        self.write_version("0.10.3")
        self.base = "refs/heads/not-present"
        self.assertIn("Git lookup failed", self.check(expected=1).stderr)

    def test_explicit_empty_or_option_like_base_cannot_skip_comparison(self):
        self.write_version("0.10.3")
        self.base = ""
        self.assertIn("nonempty", self.check(expected=1).stderr)
        result = subprocess.run([sys.executable, str(CHECKER), "--base-ref=--help"],
                                cwd=self.repo, env=self.env, text=True, capture_output=True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("nonempty", result.stderr)

    def test_manual_check_can_omit_comparison_and_use_explicit_repo_root(self):
        result = subprocess.run([sys.executable, str(CHECKER), "--repo-root", str(self.repo)],
                                cwd=self.repo / FIRMWARE, env=self.env, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("base comparison not requested", result.stdout)

    def test_missing_current_version_file_is_a_concise_failure(self):
        (self.repo / FIRMWARE / "CMakeLists.txt").unlink()
        self.assertIn("check failed", self.check(expected=1).stderr)

    def make_build(self, version="0.10.3"):
        build = self.repo / "build-fixture"
        (build / "config").mkdir(parents=True)
        (build / "project_description.json").write_text(json.dumps({
            "project_name": "ryzobee_rootmaker", "project_version": version,
            "target": "esp32s3", "app_bin": "ryzobee_rootmaker.bin",
        }), encoding="utf-8")
        (build / "config/sdkconfig.json").write_text(json.dumps({
            "APP_PROJECT_VER_FROM_CONFIG": False, "APP_EXCLUDE_PROJECT_VER_VAR": False,
        }), encoding="utf-8")
        # Independently construct the public ESP-IDF image format: header (24),
        # first segment header (8), and esp_app_desc_t (256) at offset 0x20.
        image = bytearray(288)
        image[0:2] = bytes([0xE9, 1])
        struct.pack_into("<H", image, 12, 9)  # ESP32-S3 chip ID
        struct.pack_into("<II", image, 24, 0x3C000020, 256)
        struct.pack_into("<I", image, 32, 0xABCD5432)
        image[48:48 + len(version)] = version.encode("ascii")
        image[80:97] = b"ryzobee_rootmaker"
        (build / "ryzobee_rootmaker.bin").write_bytes(image)
        return build

    def test_built_descriptor_must_match_bumped_source(self):
        self.write_version("0.10.3")
        build = self.make_build()
        result = self.check("--build-dir", str(build))
        self.assertIn("image", result.stdout)

    def test_stale_binary_fails_even_when_description_json_matches(self):
        self.write_version("0.10.3")
        build = self.make_build()
        binary = build / "ryzobee_rootmaker.bin"
        data = bytearray(binary.read_bytes())
        data[48:54] = b"0.10.2"
        binary.write_bytes(data)
        self.assertIn("image version", self.check("--build-dir", str(build), expected=1).stderr)

    def test_stale_build_description_is_rejected(self):
        self.write_version("0.10.3")
        build = self.make_build("0.10.2")
        self.assertIn("project_version", self.check("--build-dir", str(build), expected=1).stderr)

    def test_configuration_must_explicitly_disable_both_overrides(self):
        self.write_version("0.10.3")
        build = self.make_build()
        path = build / "config/sdkconfig.json"
        original = path.read_text(encoding="utf-8")
        for key in ("APP_PROJECT_VER_FROM_CONFIG", "APP_EXCLUDE_PROJECT_VER_VAR"):
            for value in (True, 0, None, "false", "missing"):
                with self.subTest(key=key, value=value):
                    data = json.loads(original)
                    if value == "missing":
                        del data[key]
                    else:
                        data[key] = value
                    path.write_text(json.dumps(data), encoding="utf-8")
                    self.assertIn(key, self.check("--build-dir", str(build), expected=1).stderr)

    def test_unused_config_version_is_not_a_second_source(self):
        self.write_version("0.10.3")
        build = self.make_build()
        self.replace_json(build / "config/sdkconfig.json", APP_PROJECT_VER="unused-old-value")
        self.check("--build-dir", str(build))

    def test_version_txt_cannot_override_source(self):
        self.write_version("0.10.3")
        build = self.make_build()
        (self.repo / FIRMWARE / "version.txt").write_text("0.10.2\n", encoding="utf-8")
        self.assertIn("version.txt", self.check("--build-dir", str(build), expected=1).stderr)

    def test_consistent_version_txt_is_allowed(self):
        self.write_version("0.10.3")
        build = self.make_build()
        (self.repo / FIRMWARE / "version.txt").write_text("0.10.3\n", encoding="utf-8")
        self.check("--build-dir", str(build))

    def test_missing_or_malformed_build_json_fails_closed(self):
        self.write_version("0.10.3")
        build = self.make_build()
        path = build / "project_description.json"
        for source in ("not json", "[]", "null", "{}", '{"project_version":"0.10.3","project_version":"0.10.2"}'):
            with self.subTest(source=source):
                path.write_text(source, encoding="utf-8")
                self.assertIn("check failed", self.check("--build-dir", str(build), expected=1).stderr)
        path.unlink()
        self.assertIn("check failed", self.check("--build-dir", str(build), expected=1).stderr)

    def test_different_project_or_target_build_is_rejected(self):
        self.write_version("0.10.3")
        build = self.make_build()
        path = build / "project_description.json"
        original = path.read_text(encoding="utf-8")
        for changes in ({"project_name": "bootloader"}, {"target": "esp32c3"}):
            with self.subTest(changes=changes):
                path.write_text(original, encoding="utf-8")
                self.replace_json(path, **changes)
                self.assertIn("not the", self.check("--build-dir", str(build), expected=1).stderr)

    def test_app_bin_path_must_stay_inside_build(self):
        self.write_version("0.10.3")
        build = self.make_build()
        for value in ("../other.bin", "/tmp/other.bin", "", None, 7):
            with self.subTest(value=value):
                self.replace_json(build / "project_description.json", app_bin=value)
                self.assertIn("app_bin", self.check("--build-dir", str(build), expected=1).stderr)

    def test_missing_binary_is_rejected(self):
        self.write_version("0.10.3")
        build = self.make_build()
        (build / "ryzobee_rootmaker.bin").unlink()
        self.assertIn("check failed", self.check("--build-dir", str(build), expected=1).stderr)

    def test_invalid_image_structure_is_rejected(self):
        self.write_version("0.10.3")
        build = self.make_build()
        binary = build / "ryzobee_rootmaker.bin"
        original = binary.read_bytes()
        for label, offset, replacement in (
            ("header magic", 0, b"\x00"), ("zero segments", 1, b"\x00"),
            ("too many segments", 1, b"\x11"), ("other chip", 12, b"\x00\x00"),
            ("short segment", 28, struct.pack("<I", 128)),
            ("truncated segment", 28, struct.pack("<I", 257)),
            ("descriptor magic", 32, b"\x00\x00\x00\x00"),
            ("other app", 80, b"x"), ("non-ASCII version", 48, b"\xff"),
            ("no version terminator", 48, b"X" * 32),
            ("no project terminator", 80, b"X" * 32),
        ):
            with self.subTest(label=label):
                image = bytearray(original)
                image[offset:offset + len(replacement)] = replacement
                binary.write_bytes(image)
                self.assertIn("check failed", self.check("--build-dir", str(build), expected=1).stderr)
        binary.write_bytes(original[:80])
        self.assertIn("truncated", self.check("--build-dir", str(build), expected=1).stderr)

    def test_source_only_increment_cannot_pass_with_excluded_image_version(self):
        self.write_version("0.10.3")
        build = self.make_build()
        binary = build / "ryzobee_rootmaker.bin"
        image = bytearray(binary.read_bytes())
        image[48:80] = bytes(32)
        binary.write_bytes(image)
        self.assertIn("image version", self.check("--build-dir", str(build), expected=1).stderr)


if __name__ == "__main__":
    unittest.main()
