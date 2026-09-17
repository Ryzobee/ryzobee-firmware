"""Real visible-change policy and cache; no LVGL pixels or device timing."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class V5RenderDirtyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-v5-dirty-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "dirty-test"
        includes = [ROOT / "tests/workbench_scripts_stubs"]
        for name in ("ryz_board", "ryz_system_ui", "ryz_apps",
                     "ryz_script_store", "ryz_script_metadata"):
            includes += [ROOT / "components" / name / "include"]
        command = [os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g",
                   "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined"]
        command += [part for path in includes for part in ("-I", str(path))]
        command += [str(ROOT / "tests/v5_render_dirty_test.c"),
                    str(ROOT / "components/ryz_system_ui/ryz_v5_render_dirty.c"),
                    str(ROOT / "components/ryz_system_ui/ryz_v5_status.c"),
                    "-o", str(cls.binary)]
        subprocess.run(command, check=True, timeout=60)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("V5_RENDER_DIRTY_PASS " + name, result.stdout)

    def test_hidden_uptime_keeps_settings_and_apps_quiet_but_is_published(self):
        self.run_case("uptime")

    def test_home_metrics_do_not_repaint_other_routes_or_wifi_nested_system(self):
        self.run_case("home-metrics")

    def test_shared_header_and_settings_visible_fields_still_repaint(self):
        self.run_case("header")

    def test_wifi_business_errors_admission_and_ap_qr_identity_still_repaint(self):
        self.run_case("network")

    def test_ble_availability_pairing_and_peer_changes_still_repaint(self):
        self.run_case("ble")

    def test_ota_identity_admission_progress_errors_and_info_uptime_still_repaint(self):
        self.run_case("version")

    def test_scripts_generations_selection_deletion_and_recovery_still_repaint(self):
        self.run_case("scripts")

    def test_hidden_updates_are_fully_published_and_legacy_setter_is_unchanged(self):
        self.run_case("publication")

    def test_stable_models_are_quiet_and_unknown_inputs_require_repaint(self):
        self.run_case("fallback")


if __name__ == "__main__":
    unittest.main()
