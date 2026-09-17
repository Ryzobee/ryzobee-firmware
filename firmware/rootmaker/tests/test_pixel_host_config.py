"""Check the narrowly allowed target/Host routing difference, without an SDK."""
import importlib.util
from pathlib import Path
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "tools/check_pixel_host_config.py"
SPEC = importlib.util.spec_from_file_location("pixel_config", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class PixelConfigContractTest(unittest.TestCase):
    def setUp(self):
        self.host = {
            "CONFIG_LV_TXT_ENC_UTF8": "1",
            "CONFIG_LV_TXT_ENC": "LV_TXT_ENC_UTF8",
            "LV_TXT_ENC": "CONFIG_LV_TXT_ENC",
            "LV_MEM_SIZE": "CONFIG_LV_MEM_SIZE",
            "CONFIG_LV_MEM_SIZE_KILOBYTES": "64",
            "LV_MEM_POOL_EXPAND_SIZE": "0",
            "LV_USE_STDLIB_MALLOC": "LV_STDLIB_BUILTIN",
            "LV_COLOR_DEPTH": "16",
        }
        self.target = {
            **self.host,
            "LV_MEM_POOL_ALLOC": "ryz_lvgl_memory_pool",
            "LV_MEM_POOL_INCLUDE": '"ryz_lvgl_memory.h"',
        }

    def test_exact_routing_is_reported_without_mutating_inputs(self):
        before = (dict(self.target), dict(self.host))
        self.assertEqual(MODULE.compare_lvgl_macros(self.target, self.host), {
            "LV_MEM_POOL_ALLOC": {"target": "ryz_lvgl_memory_pool", "host": None},
            "LV_MEM_POOL_INCLUDE": {"target": '"ryz_lvgl_memory.h"', "host": None},
        })
        self.assertEqual((self.target, self.host), before)

    def test_missing_or_changed_target_routing_is_not_exempt(self):
        for key in ("LV_MEM_POOL_ALLOC", "LV_MEM_POOL_INCLUDE"):
            for value in (None, "another_pool", ""):
                with self.subTest(key=key, value=value):
                    target = dict(self.target)
                    if value is None:
                        del target[key]
                    else:
                        target[key] = value
                    with self.assertRaisesRegex(RuntimeError, "Unexpected LVGL pool routing"):
                        MODULE.compare_lvgl_macros(target, self.host)

    def test_host_must_not_define_either_target_route_macro(self):
        for key in ("LV_MEM_POOL_ALLOC", "LV_MEM_POOL_INCLUDE"):
            for value in (self.target[key], "", "another_pool"):
                with self.subTest(key=key, value=value):
                    with self.assertRaisesRegex(RuntimeError, "Unexpected LVGL pool routing"):
                        MODULE.compare_lvgl_macros(self.target, {**self.host, key: value})

    def test_other_memory_render_and_unknown_macro_differences_still_fail(self):
        for key in ("LV_MEM_SIZE", "CONFIG_LV_MEM_SIZE_KILOBYTES", "LV_MEM_POOL_EXPAND_SIZE",
                    "LV_USE_STDLIB_MALLOC", "LV_COLOR_DEPTH", "LV_NEW_SETTING"):
            with self.subTest(key=key):
                with self.assertRaisesRegex(RuntimeError, "LVGL configuration mismatch"):
                    MODULE.compare_lvgl_macros(self.target, {**self.host, key: "changed"})

    def test_identically_wrong_text_encoding_cannot_pass_comparison(self):
        for key, value in (("CONFIG_LV_TXT_ENC_UTF8", "0"),
                           ("CONFIG_LV_TXT_ENC", "LV_TXT_ENC_ASCII"),
                           ("LV_TXT_ENC", "LV_TXT_ENC_ASCII"),
                           ("CONFIG_LV_TXT_ENC_ASCII", "1")):
            with self.subTest(key=key):
                with self.assertRaisesRegex(RuntimeError, "UTF-8|ASCII"):
                    MODULE.compare_lvgl_macros({**self.target, key: value},
                                               {**self.host, key: value})
        for key in ("CONFIG_LV_TXT_ENC_UTF8", "CONFIG_LV_TXT_ENC", "LV_TXT_ENC"):
            with self.subTest(missing=key):
                target = dict(self.target)
                del target[key]
                with self.assertRaisesRegex(RuntimeError, "UTF-8"):
                    MODULE.compare_lvgl_macros(target, self.host)


if __name__ == "__main__":
    unittest.main()
