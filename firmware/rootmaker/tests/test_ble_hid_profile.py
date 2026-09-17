"""Real private HID Profile Module with only NimBLE transport replaced.

These regressions do not establish first-discovery compatibility with iOS.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BleHidProfileTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="ryz-hid-profile-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / "hid-profile"
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-I" + str(ROOT / "tests/hid_profile_stubs"),
            "-I" + str(ROOT / "components/ryz_ble"),
            str(ROOT / "components/ryz_ble/ryz_ble_hid_profile.c"),
            str(ROOT / "tests/ble_hid_profile_test.c"), "-o", str(cls.binary),
        ], check=True, timeout=30)

    def case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True,
                                timeout=10, env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("RYZ_HID_PROFILE_PASS", result.stdout)

    def test_real_gatt_database_and_secure_descriptors(self): self.case("database")
    def test_report_map_is_consumer_only_and_exact_seven_bits(self): self.case("map")
    def test_explicit_press_and_bounded_release(self): self.case("tap")
    def test_invalid_busy_unauthorized_and_unsubscribed_taps(self): self.case("gates")
    def test_release_request_and_duplicate_are_idempotent(self): self.case("release")
    def test_suspend_resume_and_invalid_control_writes(self): self.case("suspend")
    def test_release_failure_remains_fatal_until_physical_reset(self): self.case("release_fail")
    def test_press_failure_and_allocation_failure_fail_closed(self): self.case("press_fail")
    def test_revocation_or_unsubscribe_while_pressed_requires_disconnect(self): self.case("revoke")
    def test_reused_connection_cannot_retain_report_or_subscription(self): self.case("reset")
    def test_unknown_battery_or_identity_is_never_fabricated(self): self.case("unknown")
    def test_battery_update_and_notification_respect_subscription(self): self.case("battery")
    def test_assigned_identity_serialization_and_invalid_values(self): self.case("identity")
    def test_registration_errors_are_propagated(self): self.case("registration")


if __name__ == "__main__": unittest.main()
