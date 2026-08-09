#!/usr/bin/env python3
"""Repair a development ChatESP device and check its display start records."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

if __package__:
    from tools.watch_monitor import (
        SerialRedactor,
        close_safe_serial,
        open_safe_serial,
        redact_serial_text,
    )
else:
    from watch_monitor import (  # type: ignore[no-redef]
        SerialRedactor,
        close_safe_serial,
        open_safe_serial,
        redact_serial_text,
    )


_APP_VERSION = re.compile(r"App version:\s+([^\s]+)")
_NONZERO_BRIGHTNESS = re.compile(
    r"set brightness to ([1-9][0-9]*)%"
)
_FATAL_RECORDS = (
    "Display start failed",
    "Voice runtime start failed",
    "Guru Meditation Error",
    "abort() was called",
)


@dataclass(frozen=True)
class BootDiagnosis:
    """Give a bounded result for privacy-safe display start records."""

    issues: tuple[str, ...]
    app_version: str | None
    brightness_percent: int | None

    @property
    def passed(self) -> bool:
        return not self.issues


def diagnose_boot(log: str, expected_version: str) -> BootDiagnosis:
    """Check one boot log without accepting command success as a pixel test."""
    issues: list[str] = []
    version_match = _APP_VERSION.search(log)
    app_version = version_match.group(1) if version_match else None
    if app_version is None:
        issues.append("The boot log has no application version.")
    elif app_version not in (expected_version, f"{expected_version}-dirty"):
        issues.append(
            "The device firmware does not match the current Git commit."
        )

    required_records = (
        (
            "Starting application in development mode",
            "The device did not start in development mode.",
        ),
        (
            "V2 CST820-compatible touch",
            "The boot log did not identify the V2 board.",
        ),
        (
            "Display ready at ",
            "The firmware did not report the completed display wake sequence.",
        ),
        ("Voice runtime ready", "The voice runtime did not become ready."),
    )
    for record, issue in required_records:
        if record not in log:
            issues.append(issue)

    brightness_values = [
        int(value) for value in _NONZERO_BRIGHTNESS.findall(log)
    ]
    brightness_percent = brightness_values[-1] if brightness_values else None
    if len(brightness_values) < 2:
        issues.append(
            "The startup sequence did not send the required second "
            "brightness command."
        )
    elif brightness_values[-1] != brightness_values[-2]:
        issues.append("The startup brightness commands do not agree.")

    for record in _FATAL_RECORDS:
        if record in log:
            issues.append(f"The boot log contains a fatal record: {record}.")

    return BootDiagnosis(
        tuple(issues), app_version, brightness_percent
    )


def current_git_version(root: Path) -> str:
    """Get the short commit that ESP-IDF puts in the application image."""
    result = subprocess.run(
        ["git", "rev-parse", "--short=7", "HEAD"],
        cwd=root,
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def redact_doctor_text(text: str, port: str) -> str:
    """Remove network, device, and explicit local-port identifiers."""
    safe = redact_serial_text(text)
    if not port:
        return safe
    return safe.replace(port, "[redacted local port]")


def upload_development_firmware(root: Path, port: str) -> int:
    """Build and upload through the repository policy wrapper."""
    command = [
        "uv",
        "run",
        "--locked",
        "python",
        "tools/pio.py",
        "run",
        "-e",
        "watch_dev",
        "-t",
        "upload",
        "--upload-port",
        port,
    ]
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
        sys.stdout.write(redact_doctor_text(line, port))
    return process.wait()


def collect_boot_log(port: str, duration_seconds: float) -> str:
    """Collect one redacted boot window from the explicit local port."""
    connection = open_safe_serial(port)
    redactor = SerialRedactor()
    output: list[str] = []
    try:
        deadline = time.monotonic() + duration_seconds
        while time.monotonic() < deadline:
            data = connection.read(connection.in_waiting or 1)
            if not data:
                continue
            safe = redactor.feed(data.decode("utf-8", errors="replace"))
            if safe:
                output.append(safe)
                sys.stdout.write(safe)
                sys.stdout.flush()
    finally:
        tail = redactor.finish()
        if tail:
            output.append(tail)
            sys.stdout.write(tail)
            sys.stdout.flush()
        close_safe_serial(connection)
    return "".join(output)


def run_doctor(
    root: Path,
    port: str,
    duration_seconds: float,
    *,
    upload: bool,
) -> int:
    """Repair the ChatESP device and check all automatic display start gates."""
    expected_version = current_git_version(root)
    if upload:
        print("Repair: build and upload the current development firmware.")
        if upload_development_firmware(root, port) != 0:
            print("RESULT: The development firmware upload failed.")
            return 1

    print("Check: collect one bounded device boot log.")
    try:
        log = collect_boot_log(port, duration_seconds)
    except OSError as error:
        safe_error = redact_doctor_text(str(error), port)
        print(f"RESULT: The serial port could not open: {safe_error}")
        return 1

    diagnosis = diagnose_boot(log, expected_version)
    if not diagnosis.passed:
        print("RESULT: The automatic display start checks failed.")
        for issue in diagnosis.issues:
            print(f"- {issue}")
        return 1

    print(
        "RESULT: The automatic display start checks passed "
        f"at {diagnosis.brightness_percent} percent brightness."
    )
    print(
        "PHYSICAL GATE: Confirm that CHAT ESP or READY is visible on the "
        "AMOLED. Serial records cannot prove pixel output."
    )
    return 0


def parse_args(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Upload development firmware and check the display start records."
        )
    )
    parser.add_argument("--port", required=True, help="Local ChatESP device serial port")
    parser.add_argument("--duration", type=float, default=12.0)
    parser.add_argument(
        "--no-upload",
        action="store_true",
        help="Check the installed image without a new upload.",
    )
    return parser.parse_args(arguments)


def main(arguments: list[str] | None = None) -> int:
    args = parse_args(arguments)
    if args.duration <= 0:
        print("The duration must be greater than zero.", file=sys.stderr)
        return 2
    if not args.port.strip():
        print("The serial port must not be empty.", file=sys.stderr)
        return 2
    root = Path(__file__).resolve().parents[1]
    return run_doctor(
        root,
        args.port,
        args.duration,
        upload=not args.no_upload,
    )


if __name__ == "__main__":
    raise SystemExit(main())
