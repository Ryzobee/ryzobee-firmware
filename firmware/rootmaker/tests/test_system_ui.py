"""Compile and run the system UI state machine against host display spies."""

from pathlib import Path
import subprocess
import tempfile
import unittest


class SystemUiTest(unittest.TestCase):
    def test_public_ui_contract(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix="ryzobee-system-ui-test-") as tmp:
            binary = str(Path(tmp) / "system-ui-test")
            command = [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-I" + str(root / "tests/display_stubs"),
                "-I" + str(root / "components/ryz_board/include"),
                "-I" + str(root / "components/ryz_board"),
                "-I" + str(root / "components/ryz_system_ui/include"),
                "-I" + str(root / "components/ryz_system_ui"),
                str(root / "tests/system_ui_test.c"),
                str(root / "components/ryz_system_ui/ryz_system_ui.c"),
                str(root / "components/ryz_system_ui/ryz_ui_navigation.c"),
                "-o",
                binary,
            ]
            subprocess.run(command, check=True)
            subprocess.run([binary], check=True)

    def test_production_qr_adapter_suppresses_dependency_logs(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix="ryzobee-qr-adapter-test-") as tmp:
            binary = str(Path(tmp) / "qr-adapter-test")
            command = [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-DCONFIG_LOG_DYNAMIC_LEVEL_CONTROL=1",
                "-I" + str(root / "tests/qrcode_stubs"),
                "-I" + str(root / "tests/display_stubs"),
                "-I" + str(root / "components/ryz_system_ui"),
                str(root / "tests/qr_encoder_test.c"),
                str(root / "components/ryz_system_ui/ryz_qr_encoder_esp.c"),
                "-o",
                binary,
            ]
            subprocess.run(command, check=True)
            subprocess.run([binary], check=True)


if __name__ == "__main__":
    unittest.main()
