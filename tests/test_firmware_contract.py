from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tools.firmware_contract import (
    APP_SLOT_BYTES,
    IMAGE_CEILING_BYTES,
    check_image,
    validate_irreversible_defaults,
    validate_partitions,
    validate_resolved_config,
    validate_upload_limit,
)


VALID_PARTITIONS = """\
# Name, Type, SubType, Offset, Size
nvs,data,nvs,0x9000,0x6000
otadata,data,ota,0xf000,0x2000
phy_init,data,phy,0x11000,0x1000
ota_0,app,ota_0,0x20000,0x600000
ota_1,app,ota_1,0x620000,0x600000
storage,data,spiffs,0xc20000,0x3e0000
"""

COMMON_HEADER = """\
#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 0
#define CONFIG_COMPILER_OPTIMIZATION_PERF 1
#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240 1
#define CONFIG_ESP_MAIN_TASK_STACK_SIZE 8192
#define CONFIG_FREERTOS_HZ 1000
#define CONFIG_SPIRAM 1
#define CONFIG_SPIRAM_MODE_OCT 1
#define CONFIG_SPIRAM_SPEED_80M 1
#define CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT 32
#define CONFIG_ESP_LCD_TOUCH_MAX_POINTS 1
#define CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL 1
#define CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND 0
#define CONFIG_ESPTOOLPY_FLASHMODE_QIO 1
#define CONFIG_ESPTOOLPY_FLASHFREQ_80M 1
#define CONFIG_NVS_ENCRYPTION 0
#define CONFIG_SECURE_BOOT 0
#define CONFIG_SECURE_FLASH_ENC_ENABLED 0
#define CONFIG_SECURE_ROM_DL_MODE_ENABLED 1
"""

IRREVERSIBLE_DEFAULTS = """\
CONFIG_NVS_ENCRYPTION=n
CONFIG_SECURE_BOOT=n
CONFIG_SECURE_FLASH_ENC_ENABLED=n
CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=n
CONFIG_BOOTLOADER_ANTI_ROLLBACK_ENABLE=n
CONFIG_SECURE_FLASH_PSEUDO_ROUND_FUNC=n
CONFIG_SECURE_DISABLE_ROM_DL_MODE=n
CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE=n
"""


class FirmwareContractTests(unittest.TestCase):
    def write_partitions(self, directory: str, text: str) -> Path:
        path = Path(directory) / "partitions.csv"
        path.write_text(text, encoding="utf-8")
        return path

    def test_accepts_the_reviewed_partition_shape(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            partitions = validate_partitions(
                self.write_partitions(directory, VALID_PARTITIONS)
            )

        self.assertEqual(6, len(partitions))

    def test_rejects_any_change_to_reserved_storage_or_nvs(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            changed_storage = VALID_PARTITIONS.replace(
                "storage,data,spiffs,0xc20000,0x3e0000",
                "storage,data,spiffs,0xc20000,0x3d0000",
            )
            with self.assertRaisesRegex(ValueError, "reviewed layout"):
                validate_partitions(
                    self.write_partitions(directory, changed_storage)
                )

            changed_nvs = VALID_PARTITIONS.replace(
                "nvs,data,nvs,0x9000,0x6000",
                "nvs,data,nvs,0x9000,0x5000",
            )
            with self.assertRaisesRegex(ValueError, "reviewed layout"):
                validate_partitions(
                    self.write_partitions(directory, changed_nvs)
                )

    def test_rejects_partition_flags_and_extra_fields(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            encrypted = VALID_PARTITIONS.replace(
                "ota_0,app,ota_0,0x20000,0x600000",
                "ota_0,app,ota_0,0x20000,0x600000,encrypted",
            )
            with self.assertRaisesRegex(ValueError, "flash flag"):
                validate_partitions(
                    self.write_partitions(directory, encrypted)
                )

            extra = VALID_PARTITIONS.replace(
                "nvs,data,nvs,0x9000,0x6000",
                "nvs,data,nvs,0x9000,0x6000,,extra",
            )
            with self.assertRaisesRegex(ValueError, "field count"):
                validate_partitions(self.write_partitions(directory, extra))

    def test_rejects_overlap_and_flash_overflow(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            overlap = VALID_PARTITIONS.replace(
                "storage,data,spiffs,0xc20000,0x3e0000",
                "storage,data,spiffs,0xc1f000,0x1000",
            )
            with self.assertRaisesRegex(ValueError, "overlaps"):
                validate_partitions(self.write_partitions(directory, overlap))

            overflow = VALID_PARTITIONS.replace("0x3e0000", "0x3e1000")
            with self.assertRaisesRegex(ValueError, "exceeds 16 MiB"):
                validate_partitions(self.write_partitions(directory, overflow))

    def test_rejects_wrong_ota_selection_range(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            text = VALID_PARTITIONS.replace(
                "otadata,data,ota,0xf000,0x2000",
                "otadata,data,ota,0x12000,0x2000",
            )
            with self.assertRaisesRegex(ValueError, "not safe to erase"):
                validate_partitions(self.write_partitions(directory, text))

    def test_rejects_wrong_or_mismatched_application_slots(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            wrong_size = VALID_PARTITIONS.replace(
                "ota_1,app,ota_1,0x620000,0x600000",
                "ota_1,app,ota_1,0x620000,0x5f0000",
            )
            with self.assertRaisesRegex(ValueError, "exact 6 MiB"):
                validate_partitions(
                    self.write_partitions(directory, wrong_size)
                )

            wrong_subtype = VALID_PARTITIONS.replace(
                "ota_1,app,ota_1", "ota_1,app,ota_0"
            )
            with self.assertRaisesRegex(ValueError, "exact 6 MiB"):
                validate_partitions(
                    self.write_partitions(directory, wrong_subtype)
                )

            wrong_offset = VALID_PARTITIONS.replace(
                "ota_1,app,ota_1,0x620000,0x600000",
                "ota_1,app,ota_1,0x630000,0x600000",
            ).replace(
                "storage,data,spiffs,0xc20000,0x3e0000",
                "storage,data,spiffs,0xc30000,0x3d0000",
            )
            with self.assertRaisesRegex(ValueError, "exact 6 MiB"):
                validate_partitions(
                    self.write_partitions(directory, wrong_offset)
                )

    def test_image_ceiling_keeps_twenty_percent_slot_headroom(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "firmware.bin"
            image.write_bytes(b"")
            with image.open("r+b") as stream:
                stream.truncate(IMAGE_CEILING_BYTES)
            report = check_image(image)
            self.assertEqual(IMAGE_CEILING_BYTES, report.image_bytes)
            self.assertEqual(APP_SLOT_BYTES, report.slot_bytes)
            self.assertLessEqual(report.percent, 80.0)

            with image.open("r+b") as stream:
                stream.truncate(IMAGE_CEILING_BYTES + 1)
            with self.assertRaisesRegex(ValueError, "checked ceiling"):
                check_image(image)

            image.write_bytes(b"")
            with self.assertRaisesRegex(ValueError, "empty"):
                check_image(image)

    def test_platformio_upload_limit_must_equal_one_slot(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "platformio.ini"
            config.write_text(
                "[watch_base]\nboard_upload.maximum_size = 6291456\n",
                encoding="utf-8",
            )
            validate_upload_limit(config)

            config.write_text(
                "[watch_base]\nboard_upload.maximum_size = 16777216\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "one 6 MiB slot"):
                validate_upload_limit(config)

    def test_resolved_config_requires_explicit_disabled_booleans(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / "sdkconfig.h"
            header.write_text(COMMON_HEADER, encoding="utf-8")

            validate_resolved_config(header, "watch_dev")

            header.write_text(
                COMMON_HEADER.replace(
                    "#define CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND 0\n",
                    "",
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                ValueError, "CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND"
            ):
                validate_resolved_config(header, "watch_dev")

    def test_irreversible_defaults_must_explicitly_disable_each_option(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            defaults = Path(directory) / "profile.defaults"
            defaults.write_text(IRREVERSIBLE_DEFAULTS, encoding="utf-8")
            validate_irreversible_defaults(defaults)

            defaults.write_text(
                IRREVERSIBLE_DEFAULTS.replace(
                    "CONFIG_SECURE_FLASH_PSEUDO_ROUND_FUNC=n\n", ""
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                ValueError, "CONFIG_SECURE_FLASH_PSEUDO_ROUND_FUNC"
            ):
                validate_irreversible_defaults(defaults)

    def test_resolved_config_rejects_wrong_enabled_and_numeric_values(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / "sdkconfig.h"
            header.write_text(
                COMMON_HEADER.replace(
                    "#define CONFIG_FREERTOS_HZ 1000",
                    "#define CONFIG_FREERTOS_HZ 100",
                )
                + "#define CONFIG_SECURE_BOOT 1\n",
                encoding="utf-8",
            )

            with self.assertRaisesRegex(
                ValueError,
                "CONFIG_FREERTOS_HZ.*CONFIG_SECURE_BOOT",
            ):
                validate_resolved_config(header, "watch_dev")

    def test_resolved_config_rejects_exported_irreversible_options(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / "sdkconfig.h"
            header.write_text(
                COMMON_HEADER +
                "#define CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK 1\n" +
                "#define CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE 1\n",
                encoding="utf-8",
            )

            with self.assertRaisesRegex(
                ValueError,
                "CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK.*"
                "CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE",
            ):
                validate_resolved_config(header, "watch_dev")

    def test_production_requires_its_extra_profile_values(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / "sdkconfig.h"
            header.write_text(COMMON_HEADER, encoding="utf-8")

            with self.assertRaisesRegex(
                ValueError, "CONFIG_BOOTLOADER_COMPILER_OPTIMIZATION_PERF"
            ):
                validate_resolved_config(header, "watch_prod")


if __name__ == "__main__":
    unittest.main()
