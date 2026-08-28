from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest.mock import MagicMock, patch

import serial

from tools.device_doctor import (
    collect_boot_log,
    diagnose_boot,
    main,
    otadata_region,
    redact_doctor_text,
    run_redacted_command,
    upload_development_firmware,
)


class DeviceDoctorTests(unittest.TestCase):
    def test_boot_log_closes_serial_with_the_safe_reset_sequence(self) -> None:
        connection = MagicMock()
        connection.in_waiting = 0
        connection.read.return_value = b""

        with patch(
            "tools.device_doctor.reopen_safe_serial", return_value=connection
        ), patch("tools.device_doctor.close_safe_serial") as safe_close, patch(
            "tools.device_doctor.time.monotonic", side_effect=(0.0, 1.0)
        ):
            self.assertEqual("", collect_boot_log("LOCAL_PORT", 0.5))

        safe_close.assert_called_once_with(connection)

    def test_accepts_complete_current_development_boot(self) -> None:
        log = """
I app_init: App version:      abc1234-dirty
I chatesp: Starting application in development mode
I board: Board revision: V2
I board: Touch controller: CST820-compatible
I co5300: set brightness to 65% (hardware value: 165)
I co5300: set brightness to 65% (hardware value: 165)
I chatesp: Display ready at 65 percent
I chatesp: Voice runtime ready
"""

        result = diagnose_boot(log, "abc1234")

        self.assertTrue(result.passed)
        self.assertEqual(result.app_version, "abc1234-dirty")
        self.assertEqual(result.brightness_percent, 65)

    def test_rejects_stale_image_and_one_brightness_command(self) -> None:
        log = """
I app_init: App version:      old1234
I chatesp: Starting application in development mode
I board: Board revision: V2
I board: Touch controller: CST820-compatible
I co5300: set brightness to 65% (hardware value: 165)
I chatesp: Display ready at 65 percent
I chatesp: Voice runtime ready
"""

        result = diagnose_boot(log, "new1234")

        self.assertFalse(result.passed)
        self.assertIn(
            "The device firmware does not match the current Git commit.",
            result.issues,
        )
        self.assertIn(
            "The startup sequence did not send the required second "
            "brightness command.",
            result.issues,
        )

    def test_rejects_version_with_unexpected_trailing_text(self) -> None:
        result = diagnose_boot(
            "App version: abc1234-stale\n", "abc1234"
        )

        self.assertIn(
            "The device firmware does not match the current Git commit.",
            result.issues,
        )

    def test_redacts_the_explicit_port_and_device_address(self) -> None:
        port = "/dev/cu.usbmodem-private"

        result = redact_doctor_text(
            f"Serial port {port}\nMAC: 12:34:56:78:9a:bc\n", port
        )

        self.assertNotIn(port, result)
        self.assertNotIn("12:34:56:78:9a:bc", result)
        self.assertIn("[redacted local port]", result)

    def test_rejects_an_empty_port(self) -> None:
        self.assertEqual(2, main(["--port", " "]))

    def test_rejects_missing_ready_records_and_fatal_start(self) -> None:
        result = diagnose_boot(
            "App version: abc1234\nDisplay start failed\n", "abc1234"
        )

        self.assertFalse(result.passed)
        self.assertIn(
            "The firmware did not report the completed display wake sequence.",
            result.issues,
        )
        self.assertIn(
            "The boot log contains a fatal record: Display start failed.",
            result.issues,
        )

    def test_upload_selects_the_development_ota_slot(self) -> None:
        root = Path("/repo")
        port = "LOCAL_PORT"
        with patch(
            "tools.device_doctor.upload_firmware", return_value=0
        ) as upload:
            self.assertEqual(0, upload_development_firmware(root, port))

        upload.assert_called_once_with(
            root,
            "watch_dev",
            port,
            runner=run_redacted_command,
        )

    def test_serial_reopen_error_has_one_safe_result(self) -> None:
        with patch(
            "tools.device_doctor.upload_development_firmware",
            return_value=0,
        ), patch(
            "tools.device_doctor.reopen_safe_serial",
            side_effect=serial.SerialException(
                "could not open /dev/cu.private: No such file"
            ),
        ):
            self.assertEqual(
                1,
                main(["--port", "/dev/cu.private", "--duration", "1"]),
            )

    def test_otadata_region_is_narrow_and_validated(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            firmware = root / "firmware"
            firmware.mkdir()
            partitions = firmware / "partitions.csv"
            partitions.write_text(
                "# Name, Type, SubType, Offset, Size\n"
                "nvs,data,nvs,0x9000,0x6000\n"
                "otadata,data,ota,0xf000,0x2000\n"
                "phy_init,data,phy,0x11000,0x1000\n"
                "ota_0,app,ota_0,0x20000,0x600000\n"
                "ota_1,app,ota_1,0x620000,0x600000\n"
                "storage,data,spiffs,0xc20000,0x3e0000\n",
                encoding="utf-8",
            )
            self.assertEqual((0xF000, 0x2000), otadata_region(root))

            partitions.write_text(
                "otadata,data,ota,0xf000,0x3000\n"
                "ota_0,app,ota_0,0x20000,0x600000\n"
                "ota_1,app,ota_1,0x620000,0x600000\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "not safe"):
                otadata_region(root)


if __name__ == "__main__":
    unittest.main()
