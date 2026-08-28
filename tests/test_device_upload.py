from __future__ import annotations

import unittest
import hashlib
import json
import struct
import tempfile
from pathlib import Path
from unittest.mock import MagicMock, patch

from tools.device_upload import (
    build_command,
    main,
    otadata_erase_command,
    redact_command_text,
    stage_validated_flash_command,
    upload_firmware,
)
from tools.firmware_contract import ImageReport


class DeviceUploadTests(unittest.TestCase):
    @staticmethod
    def partition_binary() -> bytes:
        type_values = {"app": 0, "data": 1}
        subtype_values = {
            "nvs": 2,
            "ota": 0,
            "phy": 1,
            "ota_0": 0x10,
            "ota_1": 0x11,
            "spiffs": 0x82,
        }
        rows = (
            ("nvs", "data", "nvs", 0x9000, 0x6000),
            ("otadata", "data", "ota", 0xF000, 0x2000),
            ("phy_init", "data", "phy", 0x11000, 0x1000),
            ("ota_0", "app", "ota_0", 0x20000, 0x600000),
            ("ota_1", "app", "ota_1", 0x620000, 0x600000),
            ("storage", "data", "spiffs", 0xC20000, 0x3E0000),
        )
        output = bytearray()
        for name, part_type, subtype, offset, size in rows:
            output.extend(
                struct.pack(
                    "<HBBII16sI",
                    0x50AA,
                    type_values[part_type],
                    subtype_values[subtype],
                    offset,
                    size,
                    name.encode().ljust(16, b"\0"),
                    0,
                )
            )
        output.extend(
            b"\xeb\xeb" + b"\xff" * 14 +
            hashlib.md5(output, usedforsecurity=False).digest()
        )
        output.extend(b"\xff" * (0xC00 - len(output)))
        return bytes(output)

    def test_commands_require_one_explicit_profile_and_port(self) -> None:
        build = build_command("watch_prod")
        erase = otadata_erase_command("LOCAL_PORT", 0xF000, 0x2000)

        self.assertEqual("watch_prod", build[build.index("-e") + 1])
        self.assertEqual(
            ["erase_region", "0xf000", "0x2000"], erase[-3:]
        )

    def test_flash_command_uses_only_staged_validated_artifacts(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "firmware" / ".pio" / "build" / "watch_dev"
            build.mkdir(parents=True)
            files = {
                "firmware.bin": b"application",
                "bootloader.bin": b"bootloader",
                "partitions.bin": self.partition_binary(),
                "ota_data_initial.bin": b"ota",
            }
            for name, data in files.items():
                (build / name).write_bytes(data)
            (build / "flasher_args.json").write_text(
                json.dumps(
                    {
                        "flash_settings": {
                            "flash_mode": "dio",
                            "flash_size": "16MB",
                            "flash_freq": "80m",
                        },
                        "flash_files": {
                            "0x0": "bootloader/bootloader.bin",
                            "0x20000": "chatesp.bin",
                            "0x8000": "partition_table/partition-table.bin",
                            "0xf000": "ota_data_initial.bin",
                        },
                    }
                ),
                encoding="utf-8",
            )
            staging = root / "stage"
            staging.mkdir()

            command = stage_validated_flash_command(
                root, "watch_dev", "LOCAL_PORT", staging
            )

            self.assertEqual("LOCAL_PORT", command[command.index("--port") + 1])
            self.assertNotIn("run", command[command.index("tools/pio.py") + 1 :])
            self.assertNotIn("upload", command)
            staged = [value for value in command if str(staging) in value]
            self.assertEqual(4, len(staged))
            for path in staged:
                self.assertTrue(Path(path).is_file())

    def test_build_is_validated_before_upload_and_narrow_erase(self) -> None:
        root = Path("/repo")
        runner = MagicMock(side_effect=(0, 0, 0))
        events: list[str] = []

        def validate(actual_root: Path, environment: str) -> ImageReport:
            self.assertEqual(root, actual_root)
            self.assertEqual("watch_dev", environment)
            events.append("validate")
            return ImageReport(100, 200)

        def record(command: list[str], actual_root: Path, port: str) -> int:
            events.append("run:" + command[-1])
            return runner(command, actual_root, port)

        with patch(
            "tools.device_upload.otadata_region", return_value=(0xF000, 0x2000)
        ), patch(
            "tools.device_upload.freeze_validated_flash_command",
            side_effect=lambda *args: (validate(args[0], args[1]), ["flash"]),
        ):
            result = upload_firmware(
                root, "watch_dev", "LOCAL_PORT", runner=record
            )

        self.assertEqual(0, result)
        self.assertEqual(
            ["run:watch_dev", "validate", "run:flash", "run:0x2000"],
            events,
        )
        self.assertEqual(3, runner.call_count)

    def test_staging_rejects_changed_partition_binary_and_large_ota_data(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "firmware" / ".pio" / "build" / "watch_dev"
            build.mkdir(parents=True)
            (build / "firmware.bin").write_bytes(b"application")
            (build / "bootloader.bin").write_bytes(b"bootloader")
            (build / "partitions.bin").write_bytes(b"not a partition table")
            (build / "ota_data_initial.bin").write_bytes(b"ota")
            (build / "flasher_args.json").write_text(
                json.dumps(
                    {
                        "flash_settings": {
                            "flash_mode": "dio",
                            "flash_size": "16MB",
                            "flash_freq": "80m",
                        },
                        "flash_files": {
                            "0x0": "bootloader/bootloader.bin",
                            "0x20000": "chatesp.bin",
                            "0x8000": "partition_table/partition-table.bin",
                            "0xf000": "ota_data_initial.bin",
                        },
                    }
                ),
                encoding="utf-8",
            )
            staging = root / "stage"
            staging.mkdir()

            with self.assertRaisesRegex(ValueError, "partition binary"):
                stage_validated_flash_command(
                    root, "watch_dev", "LOCAL_PORT", staging
                )

            (build / "partitions.bin").write_bytes(self.partition_binary())
            with (build / "ota_data_initial.bin").open("r+b") as stream:
                stream.truncate(0x2001)
            with self.assertRaisesRegex(ValueError, "OTA-data"):
                stage_validated_flash_command(
                    root, "watch_dev", "LOCAL_PORT", staging
                )

    def test_partition_binary_requires_valid_md5_and_padding(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "firmware" / ".pio" / "build" / "watch_dev"
            build.mkdir(parents=True)
            (build / "firmware.bin").write_bytes(b"application")
            (build / "bootloader.bin").write_bytes(b"bootloader")
            (build / "ota_data_initial.bin").write_bytes(b"ota")
            (build / "flasher_args.json").write_text(
                json.dumps(
                    {
                        "flash_settings": {
                            "flash_mode": "dio",
                            "flash_size": "16MB",
                            "flash_freq": "80m",
                        },
                        "flash_files": {
                            "0x0": "bootloader/bootloader.bin",
                            "0x20000": "firmware.bin",
                            "0x8000": "partitions.bin",
                            "0xf000": "ota_data_initial.bin",
                        },
                    }
                ),
                encoding="utf-8",
            )
            staging = root / "stage"
            staging.mkdir()
            partition = bytearray(self.partition_binary())
            partition[0xD0] ^= 0x01
            (build / "partitions.bin").write_bytes(partition)
            with self.assertRaisesRegex(ValueError, "MD5"):
                stage_validated_flash_command(
                    root, "watch_dev", "LOCAL_PORT", staging
                )

            partition = bytearray(self.partition_binary())
            partition[-1] = 0
            (build / "partitions.bin").write_bytes(partition)
            with self.assertRaisesRegex(ValueError, "padding"):
                stage_validated_flash_command(
                    root, "watch_dev", "LOCAL_PORT", staging
                )

    def test_contract_failure_stops_before_device_write(self) -> None:
        runner = MagicMock(return_value=0)
        with patch(
            "tools.device_upload.freeze_validated_flash_command",
            side_effect=ValueError("image too large"),
        ):
            with self.assertRaisesRegex(ValueError, "image too large"):
                upload_firmware(
                    Path("/repo"), "watch_prod", "LOCAL_PORT", runner=runner
                )

        runner.assert_called_once()

    def test_output_redacts_the_explicit_port_and_device_data(self) -> None:
        port = "/dev/cu.private"
        output = redact_command_text(
            f"Port {port}; address 192.168.1.20", port
        )

        self.assertNotIn(port, output)
        self.assertNotIn("192.168.1.20", output)

    def test_cli_requires_a_nonempty_port(self) -> None:
        self.assertEqual(
            2,
            main(["--environment", "watch_dev", "--port", " "]),
        )


if __name__ == "__main__":
    unittest.main()
