#!/usr/bin/env python3
"""Check the firmware version, optionally against a Git base and built image.

Every PR must supply --base-ref; omitting it only checks the current version
format (useful for an explicitly requested workflow_dispatch build).
"""

import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import sys


FIRMWARE = Path("firmware/rootmaker")
VERSION_FILE = FIRMWARE / "CMakeLists.txt"
VERSION = r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)"


class VersionError(Exception):
    """A failed release gate, suitable for concise command-line output."""


def git(repo, *args):
    result = subprocess.run(["git", "-C", str(repo), *args], check=False,
                            capture_output=True, text=True, encoding="utf-8")
    if result.returncode:
        raise VersionError("Git lookup failed: " + result.stderr.strip())
    return result.stdout.strip()


def source_version(source, label):
    # This deliberately accepts the repository's literal, one-line declaration,
    # not arbitrary executable CMake expressions. Comments cannot supply it.
    active = re.sub(r"#\[(=*)\[.*?(?:\]\1\]|\Z)|#[^\n]*", "", source, flags=re.S)
    assignments = re.findall(r"\bset\s*\(\s*PROJECT_VER\b", active, re.I)
    matches = re.findall(r'^set\(PROJECT_VER "(' + VERSION + r')"\)$', active, re.M)
    if len(assignments) != 1 or len(matches) != 1:
        raise VersionError(f'{label}: expected exactly one canonical set(PROJECT_VER "X.Y.Z")')
    version = matches[0]
    if len(version) > 31:
        raise VersionError(f"{label}: PROJECT_VER exceeds the firmware's 31-byte field")
    return version


def json_object(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise VersionError(f"{path}: duplicate JSON key {key}")
            result[key] = value
        return result

    value = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique)
    if not isinstance(value, dict):
        raise VersionError(f"{path}: expected a JSON object")
    return value


def image_version(binary):
    # ESP-IDF v5.5.4: esp_image_header_t is 24 bytes, followed by an 8-byte
    # esp_image_segment_header_t. The first segment starts with esp_app_desc_t
    # (256 bytes, magic 0xABCD5432, version[32] at +16, project_name[32] at +48).
    # Sources: components/bootloader_support/include/esp_app_format.h and
    # components/esp_app_format/include/esp_app_desc.h. This checks the version
    # descriptor, not the image checksum, signature, or device bootability.
    with binary.open("rb") as stream:
        prefix = stream.read(288)
    if len(prefix) != 288 or prefix[0] != 0xE9 or not 1 <= prefix[1] <= 16:
        raise VersionError("app image has an invalid or truncated ESP image header")
    if struct.unpack_from("<H", prefix, 12)[0] != 9:
        raise VersionError("app image is not for ESP32-S3")
    segment_size = struct.unpack_from("<I", prefix, 28)[0]
    if segment_size < 256 or 32 + segment_size > binary.stat().st_size:
        raise VersionError("app image first segment is truncated or lacks an app descriptor")
    if struct.unpack_from("<I", prefix, 32)[0] != 0xABCD5432:
        raise VersionError("app image has no ESP-IDF app descriptor at offset 0x20")

    def c_string(offset, label):
        value = prefix[offset:offset + 32]
        if b"\0" not in value:
            raise VersionError(f"app image {label} is not NUL-terminated")
        return value.split(b"\0", 1)[0].decode("ascii")

    if c_string(80, "project name") != "ryzobee_rootmaker":
        raise VersionError("app image project name is not ryzobee_rootmaker")
    return c_string(48, "version")


def check_build(repo, build, expected):
    build = build.resolve()
    description = json_object(build / "project_description.json")
    if description.get("project_name") != "ryzobee_rootmaker" or description.get("target") != "esp32s3":
        raise VersionError("build description is not the ryzobee_rootmaker ESP32-S3 application")
    if description.get("project_version") != expected:
        raise VersionError(f"build project_version {description.get('project_version')!r} != PROJECT_VER {expected}")
    config = json_object(build / "config/sdkconfig.json")
    for key in ("APP_PROJECT_VER_FROM_CONFIG", "APP_EXCLUDE_PROJECT_VER_VAR"):
        if config.get(key) is not False:
            raise VersionError(f"build CONFIG_{key} must explicitly be disabled")
    version_txt = repo / FIRMWARE / "version.txt"
    if version_txt.exists() and version_txt.read_text(encoding="utf-8").strip() != expected:
        raise VersionError("version.txt disagrees with the canonical PROJECT_VER")
    name = description.get("app_bin")
    if not isinstance(name, str) or not name or Path(name).is_absolute():
        raise VersionError("build app_bin must be a relative path inside --build-dir")
    binary = (build / name).resolve()
    if not binary.is_relative_to(build):
        raise VersionError("build app_bin escapes --build-dir")
    actual = image_version(binary)
    if actual != expected:
        raise VersionError(f"app image version {actual!r} != PROJECT_VER {expected}")
    return binary


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-ref", help="Trusted Git base commit/ref; mandatory in PR CI")
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    parser.add_argument("--build-dir", type=Path, help="Also verify IDF metadata, configuration and app .bin")
    args = parser.parse_args(argv)
    try:
        repo = Path(git(args.repo_root, "rev-parse", "--show-toplevel"))
        current = source_version((repo / VERSION_FILE).read_text(encoding="utf-8"), "current")
        if args.base_ref is not None:
            if not args.base_ref or args.base_ref.startswith("-"):
                raise VersionError("--base-ref must be a nonempty Git commit/ref")
            base_commit = git(repo, "rev-parse", "--verify", "--end-of-options",
                              args.base_ref + "^{commit}")
            base = source_version(git(repo, "show", f"{base_commit}:{VERSION_FILE.as_posix()}"), "base")
            if tuple(map(int, current.split("."))) <= tuple(map(int, base.split("."))):
                raise VersionError(f"PROJECT_VER must increase numerically: {base} -> {current}")
            message = f"Firmware version OK: {base} -> {current}"
        else:
            message = f"Firmware version format OK: {current} (base comparison not requested)"
        if args.build_dir is not None:
            binary = check_build(repo, args.build_dir, current)
            message += f"; build metadata/configuration/app image match ({binary.name})"
        print(message)
        return 0
    except (VersionError, OSError, UnicodeError, json.JSONDecodeError) as error:
        print(f"Firmware version check failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
