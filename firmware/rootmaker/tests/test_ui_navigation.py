"""Public-API behavior checks for the real owner-only navigation module."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components/ryz_system_ui"


class UiNavigationHostTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="ryz-ui-navigation-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "ui_navigation_test"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
            "-Werror", "-pedantic", "-fsanitize=address,undefined", "-g",
            "-I", str(COMPONENT / "include"),
            str(COMPONENT / "ryz_ui_navigation.c"),
            str(ROOT / "tests/ui_navigation_test.c"), "-o", str(cls.binary),
        ], check=True, timeout=30)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True,
                                text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("UI_NAVIGATION_PASS " + name, result.stdout)

    def test_initialization_null_calls_and_full_token_identity(self):
        self.run_case("initial")

    def test_eight_level_stack_pressure_keeps_existing_history(self):
        self.run_case("stack")

    def test_reentering_same_route_never_revalidates_old_callbacks(self):
        self.run_case("same_route")

    def test_modal_blocks_page_tokens_and_rejects_late_modal_callbacks(self):
        self.run_case("modal")

    def test_root_replaces_history_and_replace_changes_only_top(self):
        self.run_case("root_replace")

    def test_invalid_empty_stale_busy_full_requests_leave_state_unchanged(self):
        self.run_case("rejected")

    def test_invalidation_preserves_history_and_reset_revokes_all_tokens(self):
        self.run_case("epochs")


if __name__ == "__main__":
    unittest.main()
