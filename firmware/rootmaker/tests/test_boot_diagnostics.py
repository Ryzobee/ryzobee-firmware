"""C-owned diagnostic resource and reset protocol checks; no device is opened.

CMake is evaluated with its registration boundary captured; the real main.c is
compiled with typed board replacements and linker-symbol fixtures. Actual IDF
resource generation/linking and hardware reset remain separate target evidence.
"""
import hashlib
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("boot_board_cli", ROOT / "tools/board_lua.py")
cli = importlib.util.module_from_spec(spec)
serial_stub = types.ModuleType("serial")
serial_stub.Serial = Mock(side_effect=AssertionError("real serial construction is forbidden"))
with patch.dict(sys.modules, {"serial": serial_stub}):
    spec.loader.exec_module(cli)


class FakeClock:
    def __init__(self):
        self.now = 0

    def monotonic(self):
        self.now += 0.025
        return self.now

    def sleep(self, duration):
        self.now += duration


class ResetPort:
    def __init__(self, old_boot="old", replies=None):
        self.old_boot = old_boot
        self.replies = iter(replies or [])
        self.lines = []
        self.requests = []
        self.transitions = []
        self.after_reset = False
        self.dtr = self.rts = False

    def __setattr__(self, name, value):
        if name in ("dtr", "rts") and hasattr(self, "after_reset"):
            self.transitions.append((name, value))
            if name == "rts" and value:
                self.after_reset = True
        object.__setattr__(self, name, value)

    def write(self, wire):
        request = json.loads(wire)
        assert request["op"] == "info" and set(request) == {"id", "op"}
        self.requests.append(request)
        if not self.after_reset:
            if self.old_boot is not None:
                self.lines.append({"id": request["id"], "ok": True, "boot_id": self.old_boot})
        else:
            reply = next(self.replies, None)
            if reply is not None:
                self.lines.append({"id": request["id"], **reply})

    def flush(self):
        pass

    def reset_input_buffer(self):
        self.lines.clear()

    def readline(self):
        if not self.lines:
            return b""
        return (cli.PREFIX + json.dumps(self.lines.pop(0)) + "\n").encode()


class BootDiagnosticsTest(unittest.TestCase):
    def test_resource_bytes_are_unchanged_and_not_in_default_user_filesystem(self):
        source = (ROOT / "main/system_boot_diag.lua").read_bytes()
        self.assertEqual(hashlib.sha256(source).hexdigest(),
                         "8e733976742a9e62e55fc0fefe007ed268eab5b3fa5db4deaf0c69a8e7a944c9")
        self.assertFalse((ROOT / "fs/boot.lua").exists())
        for script in (ROOT / "fs").rglob("*.lua"):
            self.assertNotIn(b"BOOT_LUA_PASS", script.read_bytes(), str(script))

    def test_real_main_cmake_embeds_diagnostics_only_when_enabled(self):
        with tempfile.TemporaryDirectory(prefix="ryz-boot-cmake-") as directory:
            probe = Path(directory) / "probe.cmake"
            probe.write_text('function(idf_component_register)\n'
                             '  cmake_parse_arguments(COMP "" "" "SRCS;INCLUDE_DIRS;EMBED_TXTFILES;REQUIRES" ${ARGN})\n'
                             '  message("BOOT_EMBED=${COMP_EMBED_TXTFILES}")\n'
                             'endfunction()\n'
                             f'include("{ROOT / "main/CMakeLists.txt"}")\n')
            for enabled in (False, True):
                result = subprocess.run(["cmake", f"-DCONFIG_RYZ_BOOT_DIAGNOSTICS={'ON' if enabled else 'OFF'}",
                                         "-P", str(probe)], capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                line = next(line for line in result.stderr.splitlines() if line.startswith("BOOT_EMBED="))
                self.assertEqual(line.removeprefix("BOOT_EMBED="),
                                 "system_boot_diag.lua;../scripts/display_demo.lua;../scripts/touch_demo.lua"
                                 if enabled else "")

    def test_real_main_config_passes_immutable_symbol_or_null_without_user_file_read(self):
        with tempfile.TemporaryDirectory(prefix="ryz-boot-main-") as directory:
            temporary = Path(directory)
            common = '''#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NOT_FOUND 101
#define ESP_ERR_INVALID_ARG 102
static inline void test_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGI(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
static inline const char *esp_err_to_name(esp_err_t e) { (void)e; return "STUB"; }
typedef struct { const char *base_path,*partition_label; size_t max_files; bool format_if_mount_failed; } esp_vfs_spiffs_conf_t;
esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *config);
esp_err_t ryz_display_init(void);
esp_err_t ryz_display_init_with_configuration(unsigned char rotation, unsigned char brightness);
void ryz_json_memory_init(void);
esp_err_t ryz_touch_set_rotation(unsigned char rotation);
esp_err_t ryz_touch_init(void);
esp_err_t ryz_font_init(void);
'''
            (temporary / "boot_stubs.h").write_text(common)
            (temporary / "ryz_display_settings.h").write_text('''#pragma once
#include "boot_stubs.h"
typedef struct { unsigned char rotation, sleep_index, brightness; } ryz_display_settings_value_t;
esp_err_t ryz_display_settings_load_boot(ryz_display_settings_value_t *out);
''')
            for name in ("esp_err.h", "display.h", "esp_log.h", "esp_spiffs.h", "ryz_font.h", "touch.h"):
                (temporary / name).write_text('#include "boot_stubs.h"\n')
            fixture = temporary / "fixture.c"
            fixture.write_text('''#include "boot_stubs.h"
#include "workbench.h"
#include "ryz_display_settings.h"
#include <string.h>
const char system_source[] asm("_binary_system_boot_diag_lua_start")="DIAGNOSTIC";
const char display_source[] asm("_binary_display_demo_lua_start")="DISPLAY";
const char touch_source[] asm("_binary_touch_demo_lua_start")="TOUCH";
static unsigned calls;
static bool first_frame, json_initialized, touch_mapped;
static ryz_display_settings_value_t loaded;
esp_err_t ryz_display_settings_load_boot(ryz_display_settings_value_t *out) {
    assert(calls == 0);
    loaded=(ryz_display_settings_value_t){.rotation=TEST_BOOT_ERROR ? 0 : TEST_ROTATION,
        .sleep_index=4,.brightness=TEST_BOOT_ERROR ? 50 : 80};
    *out=loaded; ++calls; return TEST_BOOT_ERROR;
}
esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *c) {
    assert(calls == (CONFIG_RYZ_BOOT_DIAGNOSTICS ? 3U : 4U));
    assert(json_initialized);
#if !CONFIG_RYZ_BOOT_DIAGNOSTICS
    assert(first_frame); /* A committed frame, not merely a created task. */
#endif
    assert(!strcmp(c->base_path,"/scripts") && !strcmp(c->partition_label,"scripts"));
    assert(c->max_files==4 && !c->format_if_mount_failed); ++calls; return ESP_OK;
}
esp_err_t ryz_display_init(void) { assert(!"boot must pass the saved hardware configuration"); return ESP_OK; }
esp_err_t ryz_display_init_with_configuration(unsigned char r,unsigned char b) {
    assert(calls == 1 && r == loaded.rotation && b == loaded.brightness);
    ++calls; return ESP_OK;
}
void ryz_json_memory_init(void) {
    assert(calls == 2 && !json_initialized && !first_frame);
    json_initialized = true; ++calls;
}
esp_err_t ryz_workbench_start_display(void) {
    assert(calls == 3 && json_initialized && !CONFIG_RYZ_BOOT_DIAGNOSTICS);
    first_frame = true; ++calls; return ESP_OK;
}
esp_err_t ryz_touch_set_rotation(unsigned char r) {
    assert(r == loaded.rotation && !touch_mapped); touch_mapped=true; ++calls; return ESP_OK;
}
esp_err_t ryz_touch_init(void) { assert(touch_mapped); ++calls; return ESP_OK; }
esp_err_t ryz_font_init(void) { ++calls; return ESP_OK; }
void ryz_workbench_prepare(void) { assert(json_initialized); ++calls; }
void ryz_workbench_start(const ryz_workbench_config_t *c) {
    assert(json_initialized && touch_mapped);
    assert(c->filesystem_ready && c->display_ready && c->touch_ready && c->font_ready);
#if CONFIG_RYZ_BOOT_DIAGNOSTICS
    assert(c->system_boot_source==system_source && c->display_boot_source==display_source && c->touch_boot_source==touch_source);
#else
    assert(!c->system_boot_source && !c->display_boot_source && !c->touch_boot_source);
#endif
    ++calls;
}
void app_main(void);
int main(void) { app_main(); assert(calls==(CONFIG_RYZ_BOOT_DIAGNOSTICS ? 8U : 9U) + CONFIG_RYZ_LUA_FREETYPE); return 0; }
''')
            cases = [(d, f, r, 0) for d, f in ((0, 0), (0, 1), (1, 0), (1, 1)) for r in range(4)]
            cases += [(0, 0, 3, 101), (0, 0, 2, 102)]
            for enabled, freetype, rotation, load_error in cases:
                binary = temporary / f"main-{enabled}-{freetype}-{rotation}-{load_error}"
                command = [os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                           "-fsanitize=address,undefined", f"-DCONFIG_RYZ_BOOT_DIAGNOSTICS={enabled}",
                           f"-DCONFIG_RYZ_LUA_FREETYPE={freetype}",
                           f"-DTEST_ROTATION={rotation}", f"-DTEST_BOOT_ERROR={load_error}",
                           "-I", str(temporary), "-I", str(ROOT / "components/ryz_workbench/include"),
                           str(ROOT / "main/main.c"), str(fixture), "-o", str(binary)]
                subprocess.run(command, check=True, timeout=30)
                subprocess.run([str(binary)], check=True, timeout=10)

    def reset_with(self, old_boot="old", replies=None):
        board = cli.Board.__new__(cli.Board)
        board.serial = ResetPort(old_boot, replies)
        board.events = []
        clock = FakeClock()
        with patch.object(cli.time, "monotonic", clock.monotonic), patch.object(cli.time, "sleep", clock.sleep):
            result = board.reset()
        return board, result

    def test_reset_product_ready_requires_new_boot_and_matching_live_info_not_diagnostic_marker(self):
        board, result = self.reset_with(replies=[
            {"id": "boot-script", "ok": True, "output": "BOOT_LUA_PASS"},
            {"ok": True, "boot_id": "old", "runtime_gateway_ready": True},
            {"id": "old-request", "ok": True, "boot_id": "new", "runtime_gateway_ready": True},
            {"ok": True, "boot_id": "new", "runtime_gateway_ready": False},
            {"ok": True, "boot_id": "new", "runtime_gateway_ready": True},
        ])
        self.assertEqual(result["boot_id"], "new")
        self.assertIs(result["runtime_gateway_ready"], True)
        self.assertNotEqual(board.serial.requests[0]["id"], result["id"])

    def test_reset_still_pulses_when_old_firmware_is_unresponsive(self):
        for old_boot in (None, "", 123):
            with self.subTest(old_boot=old_boot):
                board, result = self.reset_with(old_boot, [{"ok": True, "boot_id": "new", "runtime_gateway_ready": True}])
                self.assertEqual(result["boot_id"], "new")
                self.assertEqual(board.serial.transitions.count(("rts", True)), 1)
                self.assertEqual(board.serial.transitions[-3:], [("dtr", False), ("rts", True), ("rts", False)])

    def test_reset_times_out_on_same_boot_missing_identity_or_unready_firmware(self):
        for reply in ({"ok": True, "boot_id": "old", "runtime_gateway_ready": True},
                      {"ok": True, "runtime_gateway_ready": True},
                      {"ok": True, "boot_id": "new", "runtime_gateway_ready": 1}):
            with self.subTest(reply=reply), self.assertRaises(TimeoutError):
                self.reset_with(replies=[reply] * 100)

    def test_diagnostic_markers_are_required_only_after_an_explicit_diagnostic_entry(self):
        self.assertIsNone(cli.checked_boot_diagnostics([{"id": "info", "ok": True}]))
        for events in ([{"id": "boot-script", "ok": False}],
                       [{"id": "boot-script", "ok": True}],
                       [{"id": "boot-display", "ok": True, "output": "DISPLAY_DEMO_READY"}]):
            with self.subTest(events=events), self.assertRaises(AssertionError):
                cli.checked_boot_diagnostics(events)
        result = cli.checked_boot_diagnostics([{"id": "boot-script", "ok": True, "output": "BOOT_LUA_PASS"}])
        self.assertIs(result["boot-script"]["ok"], True)

    def test_workbench_file_acceptance_mutates_only_its_temporary_file_not_user_boot(self):
        module_spec = importlib.util.spec_from_file_location("workbench_boot_safety", ROOT / "tools/workbench_tests.py")
        workbench = importlib.util.module_from_spec(module_spec)
        with patch.dict(sys.modules, {"board_lua": cli}):
            module_spec.loader.exec_module(workbench)

        class FileBoard:
            def __init__(self):
                self.files = {"boot.lua": "USER BOOT", "hello.lua": "USER APP"}
                self.events = []
                self.closed = False
                self.mutations = []

            def call(self, operation, **fields):
                name = fields.get("name")
                if operation in ("put", "remove"):
                    assert name not in ("boot.lua", "hello.lua"), "acceptance touched a user file"
                    assert name.startswith("wb_") and name.endswith(".lua")
                    self.mutations.append((operation, name))
                if operation == "put":
                    source = fields["source"]
                    sha = hashlib.sha256(source.encode()).hexdigest()
                    if (len(source.encode()) > 16384 or "\0" in source or
                            fields.get("sha256", sha) != sha):
                        return {"ok": False}
                    self.files[name] = source
                    return {"ok": True, "sha256": sha}
                if operation == "get":
                    source = self.files[name]
                    return {"ok": True, "source": source, "sha256": hashlib.sha256(source.encode()).hexdigest()}
                if operation == "remove":
                    del self.files[name]
                    return {"ok": True}
                if operation == "list":
                    return {"ok": True, "files": [{"name": name} for name in self.files]}
                assert operation == "info"
                return {"ok": True}

            def close(self):
                self.closed = True

        board = FileBoard()
        with patch.object(workbench, "WorkbenchBoard", return_value=board), \
                patch.object(sys, "argv", ["workbench_tests.py", "--port", "HOST_ONLY", "--only",
                                           "files_checksum_unicode_limits"]), \
                contextlib.redirect_stdout(io.StringIO()):
            workbench.main()
        self.assertEqual(board.files, {"boot.lua": "USER BOOT", "hello.lua": "USER APP"})
        self.assertTrue(board.closed and board.mutations)
        self.assertEqual(len({name for _, name in board.mutations}), 1)


if __name__ == "__main__":
    unittest.main()
