"""Production display settings core; only persistence and Owner IO are controlled."""
from pathlib import Path
import subprocess
import tempfile
import unittest


class DisplaySettingsTest(unittest.TestCase):
    def test_nvs_backend(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix="ryzobee-display-nvs-") as tmp:
            binary = str(Path(tmp) / "nvs-test")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined",
                "-I" + str(root / "tests/display_settings_stubs"),
                "-I" + str(root / "host/pixels/stubs"),
                "-I" + str(root / "components/ryz_board/include"),
                "-I" + str(root / "components/ryz_display_settings/include"),
                "-I" + str(root / "components/ryz_display_settings"),
                str(root / "tests/display_settings_esp_test.c"),
                str(root / "components/ryz_display_settings/ryz_display_settings.c"),
                "-o", binary], check=True)
            subprocess.run([binary], check=True)

    def test_transactions_and_wake_gate(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix="ryzobee-display-settings-") as tmp:
            binary = str(Path(tmp) / "settings-test")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined",
                "-I" + str(root / "host/pixels/stubs"),
                "-I" + str(root / "components/ryz_board/include"),
                "-I" + str(root / "components/ryz_display_settings/include"),
                "-I" + str(root / "components/ryz_display_settings"),
                str(root / "tests/display_settings_test.c"),
                str(root / "components/ryz_display_settings/ryz_display_settings.c"),
                "-o", binary], check=True)
            for scenario in ("transaction", "unknown", "apply-failure", "sleep", "sleep-uncertain", "corrupt", "load-error",
                             "preview", "preview-save", "preview-deferred", "preview-failures"):
                with self.subTest(scenario=scenario):
                    subprocess.run([binary, scenario], check=True)


if __name__ == "__main__":
    unittest.main()
