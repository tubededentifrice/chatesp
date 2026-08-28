#!/usr/bin/env python3
"""Build, validate, upload, and select one ChatESP firmware profile."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from collections.abc import Callable
from pathlib import Path

if __package__:
    from tools.firmware_contract import (
        EXPECTED_PARTITIONS,
        IMAGE_CEILING_BYTES,
        ImageReport,
        WATCH_ENVIRONMENTS,
        otadata_region,
        validate_build,
    )
    from tools.pio import canonical_platformio_environment, device_build_lock
    from tools.watch_monitor import redact_serial_text
else:
    from firmware_contract import (  # type: ignore[no-redef]
        EXPECTED_PARTITIONS,
        IMAGE_CEILING_BYTES,
        ImageReport,
        WATCH_ENVIRONMENTS,
        otadata_region,
        validate_build,
    )
    from pio import (  # type: ignore[no-redef]
        canonical_platformio_environment,
        device_build_lock,
    )
    from watch_monitor import redact_serial_text  # type: ignore[no-redef]


CommandRunner = Callable[[list[str], Path, str], int]


def redact_command_text(text: str, port: str) -> str:
    """Remove network, device, and explicit local-port identifiers."""
    safe = redact_serial_text(text)
    return safe.replace(port, "[redacted local port]") if port else safe


def run_redacted_command(command: list[str], root: Path, port: str) -> int:
    """Run one device command without printing the local serial port."""
    process = subprocess.Popen(
        command,
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
    )
    assert process.stdout is not None
    for line in process.stdout:
        sys.stdout.write(redact_command_text(line, port))
    return process.wait()


def build_command(environment: str) -> list[str]:
    """Return the policy-checked build command for one explicit profile."""
    return [
        "uv", "run", "--locked", "python", "tools/pio.py",
        "run", "-e", environment,
    ]


def _file_digest(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _validate_partition_binary(path: Path) -> None:
    """Require the generated binary to encode the tracked partition table."""
    type_values = {"app": 0x00, "data": 0x01}
    subtype_values = {
        "nvs": 0x02,
        "ota": 0x00,
        "phy": 0x01,
        "ota_0": 0x10,
        "ota_1": 0x11,
        "spiffs": 0x82,
    }
    data = path.read_bytes()
    if len(data) != 0xC00:
        raise ValueError("The generated partition binary has an invalid size.")
    entries: list[tuple[str, int, int, int, int, int]] = []
    for offset in range(0, len(data) - 31, 32):
        magic, part_type, subtype, address, size, label, flags = struct.unpack(
            "<HBBII16sI", data[offset : offset + 32]
        )
        if magic == 0xFFFF or magic == 0xEBEB:
            break
        if magic != 0x50AA:
            raise ValueError("The generated partition binary is invalid.")
        name = label.split(b"\0", 1)[0].decode("ascii", errors="strict")
        entries.append((name, part_type, subtype, address, size, flags))
    expected = [
        (
            name,
            type_values[part_type],
            subtype_values[subtype],
            address,
            size,
            0,
        )
        for name, part_type, subtype, address, size in EXPECTED_PARTITIONS
    ]
    if entries != expected:
        raise ValueError(
            "The generated partition binary differs from the reviewed layout."
        )
    digest_offset = len(expected) * 32
    digest_record = data[digest_offset : digest_offset + 32]
    expected_digest_record = (
        b"\xeb\xeb" + b"\xff" * 14 +
        hashlib.md5(data[:digest_offset], usedforsecurity=False).digest()
    )
    if digest_record != expected_digest_record:
        raise ValueError("The generated partition binary MD5 is invalid.")
    if any(value != 0xFF for value in data[digest_offset + 32 :]):
        raise ValueError("The generated partition binary padding is invalid.")


def stage_validated_flash_command(
    root: Path,
    environment: str,
    port: str,
    staging: Path,
) -> list[str]:
    """Stage the exact built artifacts and return one no-build flash command."""
    build = root / "firmware" / ".pio" / "build" / environment
    plan = json.loads((build / "flasher_args.json").read_text(encoding="utf-8"))
    expected_settings = {
        "flash_mode": "dio",
        "flash_size": "16MB",
        "flash_freq": "80m",
    }
    accepted_plan_names = {
        "0x0": {"bootloader.bin"},
        "0x20000": {"chatesp.bin", "firmware.bin"},
        "0x8000": {"partition-table.bin", "partitions.bin"},
        "0xf000": {"ota_data_initial.bin"},
    }
    flash_files = plan.get("flash_files") if isinstance(plan, dict) else None
    if (
        not isinstance(plan, dict)
        or plan.get("flash_settings") != expected_settings
        or not isinstance(flash_files, dict)
        or set(flash_files) != set(accepted_plan_names)
        or any(
            Path(str(flash_files[offset])).name not in names
            for offset, names in accepted_plan_names.items()
        )
    ):
        raise ValueError("The generated flash plan differs from the reviewed layout.")

    exported_files = {
        "0x0": build / "bootloader.bin",
        "0x20000": build / "firmware.bin",
        "0x8000": build / "partitions.bin",
        "0xf000": build / "ota_data_initial.bin",
    }
    sizes = {offset: path.stat().st_size for offset, path in exported_files.items()}
    if not 0 < sizes["0x0"] <= 0x8000:
        raise ValueError("The bootloader artifact exceeds its flash range.")
    if not 0 < sizes["0x20000"] <= IMAGE_CEILING_BYTES:
        raise ValueError("The application artifact exceeds its checked ceiling.")
    if not 0 < sizes["0x8000"] <= 0x1000:
        raise ValueError("The partition artifact exceeds its flash range.")
    if not 0 < sizes["0xf000"] <= 0x2000:
        raise ValueError("The OTA-data artifact exceeds its flash range.")
    _validate_partition_binary(exported_files["0x8000"])

    staged_files: list[tuple[str, Path]] = []
    for offset, source in exported_files.items():
        destination = staging / f"{int(offset, 0):08x}-{source.name}"
        shutil.copyfile(source, destination)
        if _file_digest(source) != _file_digest(destination):
            raise ValueError("A staged flash artifact does not match its build output.")
        staged_files.append((offset, destination))

    return [
        "uv", "run", "--locked", "python", "tools/pio.py",
        "pkg", "exec", "--package", "tool-esptoolpy", "--",
        "esptool.py", "--chip", "esp32s3", "--port", port,
        "--after", "watchdog_reset", "write_flash",
        "--flash_mode", expected_settings["flash_mode"],
        "--flash_size", expected_settings["flash_size"],
        "--flash_freq", expected_settings["flash_freq"],
        *(value for pair in staged_files for value in (pair[0], str(pair[1]))),
    ]


def freeze_validated_flash_command(
    root: Path, environment: str, port: str, staging: Path
) -> tuple[ImageReport, list[str]]:
    """Validate and freeze one artifact set while the build lock is held."""
    project = root / "firmware"
    pio_environment = canonical_platformio_environment(
        os.environ, root, project
    )
    core_dir = Path(pio_environment["PLATFORMIO_CORE_DIR"])
    with device_build_lock(core_dir, ["run", "-e", environment]):
        report = validate_build(root, environment)
        command = stage_validated_flash_command(
            root, environment, port, staging
        )
    return report, command


def otadata_erase_command(port: str, offset: int, size: int) -> list[str]:
    """Return the narrow command that selects the newly uploaded OTA slot."""
    return [
        "uv", "run", "--locked", "python", "tools/pio.py",
        "pkg", "exec", "--package", "tool-esptoolpy", "--",
        "esptool.py", "--chip", "esp32s3", "--port", port,
        "--after", "watchdog_reset", "erase_region", hex(offset), hex(size),
    ]


def upload_firmware(
    root: Path,
    environment: str,
    port: str,
    *,
    runner: CommandRunner = run_redacted_command,
) -> int:
    """Build and validate an image before any device write, then upload it."""
    if environment not in WATCH_ENVIRONMENTS:
        raise ValueError(f"Unknown ChatESP environment: {environment}")
    if not port.strip():
        raise ValueError("The serial port must not be empty.")

    result = runner(build_command(environment), root, port)
    if result != 0:
        return result
    with tempfile.TemporaryDirectory(prefix="chatesp-upload-") as directory:
        report, flash = freeze_validated_flash_command(
            root, environment, port, Path(directory)
        )
        print(
            f"Validated {environment}: {report.image_bytes} bytes of "
            f"{report.slot_bytes} bytes ({report.percent:.1f}%)."
        )
        result = runner(flash, root, port)
        if result != 0:
            return result

    offset, size = otadata_region(root)
    print("Select the uploaded application in OTA slot 0.")
    return runner(otadata_erase_command(port, offset, size), root, port)


def parse_args(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--environment", choices=WATCH_ENVIRONMENTS, required=True
    )
    parser.add_argument(
        "--port", required=True, help="Local ChatESP device serial port"
    )
    return parser.parse_args(arguments)


def main(arguments: list[str] | None = None) -> int:
    args = parse_args(arguments)
    if not args.port.strip():
        print("The serial port must not be empty.", file=sys.stderr)
        return 2
    root = Path(__file__).resolve().parents[1]
    try:
        return upload_firmware(root, args.environment, args.port)
    except (OSError, ValueError):
        print("The firmware upload contract failed.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
