"""Compare actual target/Host preprocessor settings, not selected guessed values."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess


ROOT = Path(__file__).resolve().parents[1]
LOADING_MACROS = {
    "LV_CONF_H", "LV_CONF_PATH", "LV_CONF_SKIP", "LV_KCONFIG_IGNORE",
    "LV_CONF_KCONFIG_EXTERNAL_INCLUDE", "LV_CONF_INCLUDE_SIMPLE",
    "LV_LVGL_H_INCLUDE_SIMPLE",
}
# ryz_lvgl/CMakeLists.txt routes only the target's existing bounded builtin
# pool to PSRAM. Host uses LVGL's static pool. This is not a general exemption
# for memory configuration, and either side changing these values must fail.
TARGET_POOL_ROUTING = {
    "LV_MEM_POOL_ALLOC": "ryz_lvgl_memory_pool",
    "LV_MEM_POOL_INCLUDE": '"ryz_lvgl_memory.h"',
}
UTF8_SELECTION = {
    "CONFIG_LV_TXT_ENC_UTF8": "1",
    "CONFIG_LV_TXT_ENC": "LV_TXT_ENC_UTF8",
    "LV_TXT_ENC": "CONFIG_LV_TXT_ENC",
}


def commands(build):
    return json.loads((build / "compile_commands.json").read_text())


def config(build):
    entry = next(item for item in commands(build)
                 if item["file"].endswith("/src/font/lv_font.c"))
    original = entry.get("arguments") or shlex.split(entry["command"])
    args = []
    skip = False
    for argument in original:
        if skip:
            skip = False
        elif argument in {"-o", "-MF", "-MT", "-MQ"}:
            skip = True
        elif argument not in {"-c", "-MD", "-MMD", "-MP", entry["file"]}:
            args.append(argument)
    args += ["-E", "-dM", "-x", "c", "-include",
             str(ROOT / "managed_components/lvgl__lvgl/src/lv_conf_internal.h"),
             "/dev/null"]
    result = subprocess.run(args, cwd=entry["directory"], check=True,
                            capture_output=True, text=True, timeout=30)
    definitions = {}
    for line in result.stdout.splitlines():
        found = re.match(r"#define ((?:CONFIG_)?LV_\w+|CONFIG_RYZ_LUA_FREETYPE)(.*)", line)
        if found and found[1] not in LOADING_MACROS:
            definitions[found[1]] = found[2].strip()
    if not definitions:
        raise RuntimeError("No LVGL configuration macros found")
    return definitions


def compare_lvgl_macros(target_macros, host_macros):
    platform_differences = {}
    for key, expected in TARGET_POOL_ROUTING.items():
        if target_macros.get(key) != expected or key in host_macros:
            raise RuntimeError("Unexpected LVGL pool routing: " + json.dumps({
                key: {"expected_target": expected, "expected_host": None,
                      "target": target_macros.get(key), "host": host_macros.get(key)},
            }))
        platform_differences[key] = {"target": expected, "host": None}
    for profile, definitions in (("target", target_macros), ("host", host_macros)):
        for key, expected in UTF8_SELECTION.items():
            if definitions.get(key) != expected:
                raise RuntimeError(f"{profile}: expected UTF-8 selection {key}={expected}, "
                                   f"got {definitions.get(key)!r}")
        if "CONFIG_LV_TXT_ENC_ASCII" in definitions:
            raise RuntimeError(f"{profile}: unexpected ASCII text-encoding selection")
    differences = {
        key: {"target": target_macros.get(key), "host": host_macros.get(key)}
        for key in sorted(target_macros.keys() | host_macros.keys())
        if key not in TARGET_POOL_ROUTING and target_macros.get(key) != host_macros.get(key)
    }
    if differences:
        raise RuntimeError("LVGL configuration mismatch: " + json.dumps(differences))
    return platform_differences


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target_build", type=Path)
    parser.add_argument("host_build", type=Path)
    args = parser.parse_args()
    target, host = args.target_build.resolve(), args.host_build.resolve()
    target_macros, host_macros = config(target), config(host)
    platform_differences = compare_lvgl_macros(target_macros, host_macros)
    target_options = (target / "esp-idf/espressif__freetype/output/include/"
                      "freetype/config/ftoption.h").read_bytes()
    host_options = (host / "freetype/include/freetype/config/ftoption.h").read_bytes()
    if target_options != host_options:
        raise RuntimeError("FreeType ftoption.h differs from the target")
    expected_sources = [
        "components/ryz_font/ryz_font.c",
        "components/ryz_font/ryz_font_bitmap.c",
        "components/ryz_lvgl/ryz_lvgl_brand_font.c",
        "components/ryz_lvgl/ryz_lvgl.c",
        "components/ryz_lvgl/ryz_lua_assets.c",
        "managed_components/lvgl__lvgl/src/font/lv_font.c",
        "managed_components/lvgl__lvgl/src/draw/sw/lv_draw_sw_letter.c",
        "managed_components/espressif__freetype/freetype/src/truetype/truetype.c",
    ]
    host_commands = commands(host)
    target_compiled = {Path(item["file"]).resolve() for item in commands(target)}
    freetype_enabled = target_macros.get("CONFIG_RYZ_LUA_FREETYPE") == "1"
    runtime_font_source = ROOT / ("components/ryz_font/ryz_font.c" if freetype_enabled
                                else "components/ryz_font/ryz_font_disabled.c")
    if runtime_font_source not in target_compiled:
        raise RuntimeError("Target does not compile its selected font backend")
    if not freetype_enabled:
        expected_sources.append("components/ryz_font/ryz_font_disabled.c")
        if ROOT / "components/ryz_lvgl/ryz_lvgl_brand_font.c" in target_compiled:
            raise RuntimeError("Static target unexpectedly compiles the dynamic Lua font cache")
    # Derive native UI coverage from the firmware's actual compilation, so
    # adding a device page without linking it into the simulator fails here.
    native_sources = sorted(path for path in target_compiled
                            if path.parent == ROOT / "components/ryz_system_ui")
    if not native_sources:
        raise RuntimeError("Target compilation has no native system UI sources")
    expected_sources += [str(path.relative_to(ROOT)) for path in native_sources]
    compiled = [Path(item["file"]).resolve() for item in host_commands]
    for source in expected_sources:
        if compiled.count(ROOT / source) != 1:
            raise RuntimeError("Expected exactly one real source compilation: " + source)
    sdl_compiled = [Path(item["file"]).resolve() for item in host_commands
                    if "CMakeFiles/v5_simulator.dir/" in
                    (item.get("output", "") + item.get("command", ""))]
    monitor_sha = None
    factory_sources = {}
    if sdl_compiled:
        for source in ("components/ryz_runtime/app_runtime.c",
                       "components/ryz_runtime/app_tools.c"):
            path = ROOT / source
            if path not in target_compiled or sdl_compiled.count(path) != 1:
                raise RuntimeError("SDL must compile the firmware Lua source: " + source)
        if ROOT / "components/ryz_monitor/ryz_monitor_stream.c" in sdl_compiled:
            raise RuntimeError("SDL factory tools must not use the legacy C Monitor policy")
        # Firmware builds Lua as separate translation units; the Host uses
        # upstream's MAKE_LIB amalgamation of those same managed sources.
        lua_dir = ROOT / "managed_components/georgik__lua/lua"
        amalgamation = lua_dir / "onelua.c"
        includes = {lua_dir / name for name in re.findall(
            r'#include "([^"]+\.c)"', amalgamation.read_text())
            if name not in {"lua.c", "luac.c"}}
        target_lua = {path for path in target_compiled if path.parent == lua_dir}
        if sdl_compiled.count(amalgamation) != 1 or not target_lua or includes != target_lua:
            raise RuntimeError("SDL Lua amalgamation differs from target Lua sources")
        for kind in ("monitor", "i2c", "hardware"):
            name = f"tool_{kind}.lua"
            factory = (ROOT / "fs" / name).read_bytes()
            generated = (host / f"{kind}_source.h").read_text()
            embedded = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2}),", generated))
            if embedded != factory:
                raise RuntimeError(f"SDL {name} source differs from the factory script")
            digest = hashlib.sha256(embedded).hexdigest()
            versions = re.findall(rb"(?m)^-- @version: ([0-9]+\.[0-9]+\.[0-9]+)$", factory)
            if len(versions) != 1:
                raise RuntimeError(f"Factory {name} needs exactly one numeric version")
            version = versions[0].decode("ascii")
            if f'{kind}_version[] = "{version}";' not in generated or \
                    f'{kind}_sha256[] = "{digest}";' not in generated:
                raise RuntimeError(f"SDL {name} metadata differs from its actual bytes")
            factory_sources[name] = {"sha256": digest, "version": version, "bytes": len(embedded)}
        monitor_sha = factory_sources["tool_monitor.lua"]["sha256"]
        if not freetype_enabled:
            # Offline font tests are deliberately linked to FreeType; the SDL
            # product executable must not accidentally inherit that dependency.
            executable = host / "v5_simulator.app/Contents/MacOS/v5_simulator"
            if not executable.exists():
                executable = host / "v5_simulator"
            symbols = subprocess.run(["nm", str(executable)], check=True,
                                     capture_output=True, text=True, timeout=20).stdout
            if re.search(r"\b_?FT_\w+\b|_binary_\w+_ttf_start\b", symbols):
                raise RuntimeError("Static SDL executable contains FreeType or embedded TTFs")
    print("PIXEL_HOST_CONFIG_PASS " + json.dumps({
        "lv_macros": sum(key.startswith("LV_") for key in target_macros),
        "config_lv_macros": sum(key.startswith("CONFIG_LV_") for key in target_macros),
        "platform_differences": platform_differences,
        "text_encoding": "UTF-8",
        "freetype_ftoption_sha256": hashlib.sha256(target_options).hexdigest(),
        "lua_freetype_enabled": freetype_enabled,
        "real_sources": len(expected_sources),
        "native_ui_sources": len(native_sources),
        "sdl_monitor_sha256": monitor_sha,
        "sdl_factory_sources": factory_sources,
    }, sort_keys=True))


if __name__ == "__main__":
    main()
