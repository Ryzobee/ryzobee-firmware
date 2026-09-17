"""RootMaker's measured BLE HCI allocation regression; no hardware claim.

Internal-only Host pools failed with ESP_ERR_NO_MEM and largest block 7680 B
on the full firmware. The real-device ble_boot_probe remains the acceptance
signal. This check prevents silently restoring that known-bad board policy.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


def options(path):
    return dict(line.split("=", 1) for line in path.read_text().splitlines()
                if line.startswith("CONFIG_") and "=" in line)


class BleMemoryConfigTest(unittest.TestCase):
    def test_ancs_enables_sdk_gatt_client_without_observer_or_extra_peers(self):
        for name in ("sdkconfig", "sdkconfig.defaults"):
            with self.subTest(config=name):
                values = options(ROOT / name)
                # IDF 5.5.4 Kconfig couples GATT Client to Central capability.
                # These flags do not authorize runtime scanning or Lua config.
                self.assertEqual(values.get("CONFIG_BT_NIMBLE_ROLE_CENTRAL"), "y")
                self.assertEqual(values.get("CONFIG_BT_NIMBLE_GATT_CLIENT"), "y")
                self.assertNotEqual(values.get("CONFIG_BT_NIMBLE_ROLE_OBSERVER"), "y")
                self.assertEqual(values.get("CONFIG_BT_NIMBLE_MAX_CONNECTIONS"), "1")
                self.assertEqual(values.get("CONFIG_BT_NIMBLE_MAX_BONDS"), "1")

    def test_audited_zero_initialized_data_can_use_external_bss(self):
        for name in ("sdkconfig", "sdkconfig.defaults"):
            with self.subTest(config=name):
                values = options(ROOT / name)
                self.assertEqual(values.get("CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY"), "y")
                # A missing PSRAM must fail startup, not let statically linked
                # external data run with unavailable memory. Global malloc and
                # DMA/task-stack allocation policy remain explicit/internal.
                self.assertNotEqual(values.get("CONFIG_SPIRAM_IGNORE_NOTFOUND"), "y")
                self.assertNotEqual(values.get("CONFIG_SPIRAM_USE_MALLOC"), "y")

    def test_static_ui_stack_budget_does_not_shrink_optional_freetype(self):
        source = (ROOT / "components/ryz_workbench/workbench.c").read_text()
        self.assertIn("#if CONFIG_RYZ_LUA_FREETYPE\n"
                      "#define SYSTEM_UI_TASK_STACK_BYTES (40 * 1024)\n"
                      "#else\n#define SYSTEM_UI_TASK_STACK_BYTES (24 * 1024)\n#endif", source)
        self.assertIn('"ui_owner_stack_free_min_bytes"', source)

    def test_persistent_rx_text_buffer_reserves_internal_heap_for_radios(self):
        source = (ROOT / "components/ryz_workbench/workbench.c").read_text()
        self.assertRegex(source, r"char\s*\*line\s*=\s*heap_caps_malloc\(\s*RX_MAX\s*,\s*"
                         r"MALLOC_CAP_SPIRAM\s*\|\s*MALLOC_CAP_8BIT\s*\)")
        self.assertNotRegex(source, r"malloc\(\s*RX_MAX\s*\)")
        # UART reads into an internal task-local staging buffer, never the
        # external text line. This is an allocation-policy check, not RF proof.
        self.assertRegex(source, r"uint8_t buffer\[256\]")
        self.assertRegex(source, r"uart_read_bytes\(UART_NUM_0, buffer, sizeof\(buffer\)")

    def test_saved_and_reproducible_configs_use_external_host_memory(self):
        for name in ("sdkconfig", "sdkconfig.defaults"):
            with self.subTest(config=name):
                values = options(ROOT / name)
                self.assertEqual(values.get("CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL"), "y")
                self.assertNotEqual(values.get("CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL"), "y")
                self.assertEqual(values.get("CONFIG_SPIRAM_USE_CAPS_ALLOC"), "y")
                self.assertNotEqual(values.get("CONFIG_SPIRAM_USE_MALLOC"), "y")
                self.assertNotEqual(values.get("CONFIG_RYZ_LUA_FREETYPE"), "y")
                self.assertEqual(values.get("CONFIG_BT_NIMBLE_MAX_CONNECTIONS"), "1")
                self.assertEqual(values.get("CONFIG_BT_NIMBLE_MAX_BONDS"), "1")
                # SDK ESP32-S3 quad-PSRAM memory-saving profile. Freeze the
                # coupled defaults: enabling PSRAM alone otherwise grows DMA.
                self.assertEqual(values.get("CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP"), "y")
                self.assertEqual(values.get("CONFIG_ESP_WIFI_STATIC_TX_BUFFER"), "y")
                self.assertEqual(values.get("CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM"), "6")
                self.assertEqual(values.get("CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM"), "6")
                self.assertEqual(values.get("CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM"), "32")
                self.assertEqual(values.get("CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM"), "32")
                self.assertEqual(values.get("CONFIG_ESP_WIFI_RX_BA_WIN"), "6")


if __name__ == "__main__":
    unittest.main()
