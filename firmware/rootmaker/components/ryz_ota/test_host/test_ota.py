"""Host contract tests for the public Ryzobee OTA seam."""

from pathlib import Path
import subprocess
import tempfile
import unittest


class OtaStateTest(unittest.TestCase):
    def test_ota_state_machine(self):
        component = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix="ryzobee-ota-test-") as temporary:
            binary = Path(temporary) / "ota-state-test"
            command = [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-pthread",
                "-I" + str(component / "test_host/stubs"),
                "-I" + str(component / "include"),
                "-I" + str(component),
                str(component / "test_host/ota_state_test.c"),
                str(component / "test_host/fake_platform.c"),
                str(component / "ryz_ota.c"),
                "-o",
                str(binary),
            ]
            subprocess.run(command, check=True)
            for scenario in (
                "initial",
                "running-info",
                "gating",
                "progress",
                "wrong-project",
                "same-version",
                "empty-version",
                "cancel",
                "candidate-pending-cleanup",
                "preinit-hold",
                "hold-starting",
                "hold-downloading",
                "hold-verifying",
                "hold-cancel-error",
                "old-attempt",
                "cleanup-unknown",
                "failure",
                "disabled",
                "confirm-failure",
                "reboot-guard",
                "reboot-arguments",
                "reboot-attempt",
                "reboot-cleanup",
                "reboot-failed-active",
                "reboot-confirmation",
                "reboot-begin-pending",
                "reboot-begin-rejected",
                "reboot-platform-pending",
                "reboot-end-pending",
            ):
                with self.subTest(scenario=scenario):
                    completed = subprocess.run(
                        [str(binary), scenario],
                        text=True,
                        capture_output=True,
                        check=False,
                        timeout=5,
                    )
                    self.assertEqual(
                        completed.returncode, 0, completed.stderr + completed.stdout
                    )
                    self.assertIn("RYZ_OTA_STATE_PASS", completed.stdout)


if __name__ == "__main__":
    unittest.main()
