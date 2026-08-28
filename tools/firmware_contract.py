#!/usr/bin/env python3
"""Validate ChatESP partitions, firmware images, and resolved SDK settings."""

from __future__ import annotations

import argparse
import csv
import json
from configparser import ConfigParser, Error as ConfigError
from dataclasses import dataclass
from pathlib import Path


FLASH_BYTES = 0x1000000
APP_SLOT_BYTES = 0x600000
# Keep at least 20 percent of one OTA slot free for later verified changes.
IMAGE_CEILING_BYTES = APP_SLOT_BYTES * 4 // 5
OTA_0_OFFSET = 0x20000
OTA_1_OFFSET = 0x620000
OTADATA_OFFSET = 0xF000
OTADATA_BYTES = 0x2000
WATCH_ENVIRONMENTS = ("watch_dev", "watch_prod")

EXPECTED_PARTITIONS = (
    ("nvs", "data", "nvs", 0x9000, 0x6000),
    ("otadata", "data", "ota", OTADATA_OFFSET, OTADATA_BYTES),
    ("phy_init", "data", "phy", 0x11000, 0x1000),
    ("ota_0", "app", "ota_0", OTA_0_OFFSET, APP_SLOT_BYTES),
    ("ota_1", "app", "ota_1", OTA_1_OFFSET, APP_SLOT_BYTES),
    ("storage", "data", "spiffs", 0xC20000, 0x3E0000),
)

COMMON_CONFIG = {
    "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE": "0",
    "CONFIG_COMPILER_OPTIMIZATION_PERF": "1",
    "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240": "1",
    "CONFIG_ESP_MAIN_TASK_STACK_SIZE": "8192",
    "CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND": "0",
    "CONFIG_ESPTOOLPY_FLASHMODE_QIO": "1",
    "CONFIG_ESPTOOLPY_FLASHFREQ_80M": "1",
    "CONFIG_FREERTOS_HZ": "1000",
    "CONFIG_SPIRAM": "1",
    "CONFIG_SPIRAM_MODE_OCT": "1",
    "CONFIG_SPIRAM_SPEED_80M": "1",
    "CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT": "32",
    "CONFIG_ESP_LCD_TOUCH_MAX_POINTS": "1",
    "CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL": "1",
    "CONFIG_NVS_ENCRYPTION": "0",
    "CONFIG_SECURE_BOOT": "0",
    "CONFIG_SECURE_FLASH_ENC_ENABLED": "0",
    "CONFIG_SECURE_ROM_DL_MODE_ENABLED": "1",
}

IRREVERSIBLE_DISABLED_DEFAULTS = (
    "CONFIG_NVS_ENCRYPTION",
    "CONFIG_SECURE_BOOT",
    "CONFIG_SECURE_FLASH_ENC_ENABLED",
    "CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK",
    "CONFIG_BOOTLOADER_ANTI_ROLLBACK_ENABLE",
    "CONFIG_SECURE_FLASH_PSEUDO_ROUND_FUNC",
    "CONFIG_SECURE_DISABLE_ROM_DL_MODE",
    "CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE",
)
FORBIDDEN_RESOLVED_CONFIG = tuple(IRREVERSIBLE_DISABLED_DEFAULTS)

PROFILE_CONFIG = {
    "watch_dev": {},
    "watch_prod": {
        "CONFIG_SPIRAM_MEMTEST": "0",
        "CONFIG_BOOTLOADER_COMPILER_OPTIMIZATION_PERF": "1",
        "CONFIG_BOOTLOADER_LOG_LEVEL_WARN": "1",
    },
}


@dataclass(frozen=True)
class Partition:
    """One validated row from the ESP-IDF partition table."""

    name: str
    type: str
    subtype: str
    offset: int
    size: int

    @property
    def end(self) -> int:
        return self.offset + self.size


@dataclass(frozen=True)
class ImageReport:
    """Application image use of one OTA slot."""

    image_bytes: int
    slot_bytes: int

    @property
    def percent(self) -> float:
        return self.image_bytes * 100.0 / self.slot_bytes


def _parse_number(value: str) -> int:
    text = value.strip()
    if not text:
        raise ValueError("A partition number is empty.")
    suffix = text[-1].upper()
    if suffix in ("K", "M"):
        factor = 1024 if suffix == "K" else 1024 * 1024
        return int(text[:-1], 0) * factor
    return int(text, 0)


def read_partitions(path: Path) -> tuple[Partition, ...]:
    """Read explicit, bounded partition rows from one tracked CSV file."""
    partitions: list[Partition] = []
    with path.open(newline="", encoding="utf-8") as stream:
        for line_number, raw_row in enumerate(csv.reader(stream), start=1):
            row = [value.strip() for value in raw_row]
            if not row or not row[0] or row[0].startswith("#"):
                continue
            if len(row) not in (5, 6):
                raise ValueError(
                    f"Partition row {line_number} has an invalid field count."
                )
            if len(row) == 6 and row[5]:
                raise ValueError(
                    f"Partition {row[0]} must not have a flash flag."
                )
            try:
                partition = Partition(
                    row[0], row[1], row[2],
                    _parse_number(row[3]), _parse_number(row[4]),
                )
            except ValueError as error:
                raise ValueError(
                    f"Partition row {line_number} has an invalid number."
                ) from error
            if partition.size <= 0:
                raise ValueError(f"Partition {partition.name} has no size.")
            partitions.append(partition)
    if not partitions:
        raise ValueError("The partition table is empty.")
    return tuple(partitions)


def validate_partitions(path: Path) -> tuple[Partition, ...]:
    """Validate the complete ChatESP 16 MiB partition contract."""
    partitions = read_partitions(path)
    names: set[str] = set()
    previous_end = 0x9000
    for partition in sorted(partitions, key=lambda item: item.offset):
        if partition.name in names:
            raise ValueError(f"Partition name {partition.name} is duplicated.")
        names.add(partition.name)
        alignment = 0x10000 if partition.type == "app" else 0x1000
        if partition.offset % alignment != 0:
            raise ValueError(f"Partition {partition.name} has invalid alignment.")
        if partition.offset < previous_end:
            raise ValueError(f"Partition {partition.name} overlaps an earlier row.")
        if partition.end > FLASH_BYTES:
            raise ValueError(f"Partition {partition.name} exceeds 16 MiB flash.")
        previous_end = partition.end

    by_name = {partition.name: partition for partition in partitions}
    for name, subtype, offset in (
        ("ota_0", "ota_0", OTA_0_OFFSET),
        ("ota_1", "ota_1", OTA_1_OFFSET),
    ):
        partition = by_name.get(name)
        if (
            partition is None
            or partition.type != "app"
            or partition.subtype != subtype
            or partition.offset != offset
            or partition.size != APP_SLOT_BYTES
        ):
            raise ValueError(
                f"Partition {name} must match its exact 6 MiB application slot."
            )
    otadata = by_name.get("otadata")
    if (
        otadata is None
        or otadata.type != "data"
        or otadata.subtype != "ota"
        or otadata.offset != OTADATA_OFFSET
        or otadata.size != OTADATA_BYTES
    ):
        raise ValueError("The OTA selection partition is not safe to erase.")
    actual = tuple(
        (item.name, item.type, item.subtype, item.offset, item.size)
        for item in partitions
    )
    if actual != EXPECTED_PARTITIONS:
        raise ValueError(
            "The partition definition differs from the reviewed layout."
        )
    return partitions


def otadata_region(root: Path) -> tuple[int, int]:
    """Return the exact OTA selection range after complete validation."""
    validate_partitions(root / "firmware" / "partitions.csv")
    return OTADATA_OFFSET, OTADATA_BYTES


def validate_upload_limit(path: Path) -> None:
    """Require PlatformIO to reject an image larger than one OTA slot."""
    parser = ConfigParser(interpolation=None)
    with path.open(encoding="utf-8") as stream:
        parser.read_file(stream)
    try:
        maximum_size = _parse_number(
            parser.get("watch_base", "board_upload.maximum_size")
        )
    except (ConfigError, ValueError) as error:
        raise ValueError("The PlatformIO upload size limit is invalid.") from error
    if maximum_size != APP_SLOT_BYTES:
        raise ValueError("The PlatformIO upload limit must equal one 6 MiB slot.")


def check_image(image: Path, slot_bytes: int = APP_SLOT_BYTES) -> ImageReport:
    """Require one firmware image to fit the checked headroom ceiling."""
    image_bytes = image.stat().st_size
    report = ImageReport(image_bytes, slot_bytes)
    if image_bytes == 0:
        raise ValueError("The firmware image is empty.")
    if image_bytes > IMAGE_CEILING_BYTES:
        raise ValueError(
            f"Firmware image is {image_bytes} bytes; checked ceiling is "
            f"{IMAGE_CEILING_BYTES} bytes."
        )
    return report


def read_resolved_config(path: Path) -> dict[str, str]:
    """Read explicit values from an ESP-IDF generated config artifact."""
    if path.suffix == ".json":
        raw = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(raw, dict):
            raise ValueError("The resolved SDK configuration is invalid.")
        values: dict[str, str] = {}
        for name, value in raw.items():
            if isinstance(value, bool):
                normalized = "1" if value else "0"
            elif isinstance(value, (int, str)):
                normalized = str(value)
            else:
                continue
            values[f"CONFIG_{name}"] = normalized
        return values

    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("#define CONFIG_"):
            continue
        parts = line.split(maxsplit=2)
        if len(parts) == 3:
            values[parts[1]] = parts[2]
    return values


def validate_resolved_config(path: Path, environment: str) -> None:
    """Check the resolved build values for one reviewed device profile."""
    if environment not in WATCH_ENVIRONMENTS:
        raise ValueError(f"Unknown ChatESP environment: {environment}")
    values = read_resolved_config(path)
    expected = COMMON_CONFIG | PROFILE_CONFIG[environment]
    errors = []
    for name, value in expected.items():
        resolved = values.get(name)
        if resolved != value:
            errors.append(f"{name} must resolve to {value}")
    for name in FORBIDDEN_RESOLVED_CONFIG:
        if values.get(name) == "1":
            errors.append(f"{name} must not resolve to 1")
    if errors:
        raise ValueError("; ".join(errors))


def validate_irreversible_defaults(path: Path) -> None:
    """Require every reviewed irreversible option to be explicitly disabled."""
    values: dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        name, value = line.split("=", 1)
        values[name.strip()] = value.strip()
    errors = [
        f"{name} must be explicitly disabled"
        for name in IRREVERSIBLE_DISABLED_DEFAULTS
        if values.get(name) != "n"
    ]
    if errors:
        raise ValueError("; ".join(errors))


def validate_build(root: Path, environment: str) -> ImageReport:
    """Validate partitions, resolved settings, and one built application image."""
    validate_partitions(root / "firmware" / "partitions.csv")
    validate_upload_limit(root / "firmware" / "platformio.ini")
    validate_irreversible_defaults(
        root / "firmware" / "config" / f"{environment}.defaults"
    )
    build = root / "firmware" / ".pio" / "build" / environment
    validate_resolved_config(
        build / "config" / "sdkconfig.json", environment
    )
    return check_image(build / "firmware.bin")


def parse_args(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--environment", choices=WATCH_ENVIRONMENTS, required=True
    )
    parser.add_argument("--root", type=Path)
    return parser.parse_args(arguments)


def main(arguments: list[str] | None = None) -> int:
    args = parse_args(arguments)
    root = (
        args.root.expanduser().resolve()
        if args.root is not None
        else Path(__file__).resolve().parents[1]
    )
    try:
        report = validate_build(root, args.environment)
    except (OSError, ValueError) as error:
        print(f"Firmware contract failed: {error}")
        return 1
    print(
        f"{args.environment} application: {report.image_bytes} bytes of "
        f"{report.slot_bytes} bytes ({report.percent:.1f}%)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
