#!/usr/bin/env python3
"""Read the ESP32-S3 USB log without an open-time loader reset."""

from __future__ import annotations

import argparse
import math
import re
import sys
import time
from collections.abc import Callable

import serial


_MAXIMUM_SERIAL_LINE_CHARS = 4_096
_MAC_ADDRESS = re.compile(
    r"(?i)(?<![0-9a-f])(?:[0-9a-f]{2}:){5}[0-9a-f]{2}(?![0-9a-f])"
)
_IPV4_ADDRESS = re.compile(
    r"(?<![0-9])(?:25[0-5]|2[0-4][0-9]|1?[0-9]{1,2})"
    r"(?:\.(?:25[0-5]|2[0-4][0-9]|1?[0-9]{1,2})){3}(?![0-9])"
)
_IPV6_ADDRESS = re.compile(
    r"(?i)(?<![0-9a-f:])(?:[0-9a-f]{0,4}:){2,7}[0-9a-f]{0,4}"
    r"(?![0-9a-f:])"
)
_CONNECTED_NETWORK = re.compile(r"(?i)(connected with )([^,\r\n]+)")
_SSID_VALUE = re.compile(
    r"(?i)((?<![a-z0-9_])ssid[ \t]*[:=][ \t]*)([^,\r\n]+)"
)
_LATENCY_FIELD = re.compile(r"\b([a-z_]+_ms)=([0-9]+)\b")


class LatencySummary:
    """Collect privacy-safe latency fields and calculate p50 and p90."""

    def __init__(self) -> None:
        self._values: dict[str, list[int]] = {}

    def add_line(self, line: str) -> None:
        if "LATENCY " not in line:
            return
        for name, value in _LATENCY_FIELD.findall(line):
            self._values.setdefault(name, []).append(int(value))

    @staticmethod
    def _percentile(values: list[int], percentile: float) -> int:
        ordered = sorted(values)
        index = max(0, math.ceil(percentile * len(ordered)) - 1)
        return ordered[index]

    def report(self) -> str:
        lines = ["Latency summary (milliseconds):"]
        for name in sorted(self._values):
            values = self._values[name]
            lines.append(
                f"{name}: n={len(values)} "
                f"p50={self._percentile(values, 0.50)} "
                f"p90={self._percentile(values, 0.90)}"
            )
        return "\n".join(lines) if self._values else "No latency events found."


def redact_serial_text(text: str) -> str:
    """Remove local network identifiers from diagnostic output."""
    text = _CONNECTED_NETWORK.sub(r"\1[redacted network]", text)
    text = _SSID_VALUE.sub(r"\1[redacted network]", text)
    text = _MAC_ADDRESS.sub("[redacted address]", text)
    text = _IPV4_ADDRESS.sub("[redacted address]", text)
    return _IPV6_ADDRESS.sub("[redacted address]", text)


class SerialRedactor:
    """Redact complete serial lines and discard an overlong line."""

    def __init__(self) -> None:
        self._pending: list[str] = []
        self._dropping_overlong_line = False

    def feed(self, text: str) -> str:
        output: list[str] = []
        for character in text:
            if self._dropping_overlong_line:
                if character == "\n":
                    self._dropping_overlong_line = False
                continue
            if character == "\n":
                output.append(redact_serial_text("".join(self._pending) + "\n"))
                self._pending.clear()
                continue
            self._pending.append(character)
            if len(self._pending) > _MAXIMUM_SERIAL_LINE_CHARS:
                output.append("[redacted overlong serial line]\n")
                self._pending.clear()
                self._dropping_overlong_line = True
        return "".join(output)

    def finish(self) -> str:
        if self._dropping_overlong_line or not self._pending:
            self._pending.clear()
            return ""
        output = redact_serial_text("".join(self._pending))
        self._pending.clear()
        return output


def open_safe_serial(
    port: str,
    factory: Callable[[], serial.Serial] = serial.Serial,
) -> serial.Serial:
    """Set inactive control lines before the serial port opens."""
    connection = factory()
    connection.port = port
    connection.baudrate = 115_200
    connection.timeout = 0.2
    connection.dtr = False
    connection.rts = False
    connection.open()
    return connection


def monitor(
    port: str, duration_seconds: float, *, latency_report: bool = False
) -> int:
    connection = open_safe_serial(port)
    redactor = SerialRedactor()
    latency = LatencySummary()
    try:
        deadline = time.monotonic() + duration_seconds
        while time.monotonic() < deadline:
            data = connection.read(connection.in_waiting or 1)
            if data:
                output = redactor.feed(data.decode("utf-8", errors="replace"))
                if output:
                    for line in output.splitlines():
                        latency.add_line(line)
                    sys.stdout.write(output)
                    sys.stdout.flush()
    finally:
        output = redactor.finish()
        if output:
            latency.add_line(output)
            sys.stdout.write(output)
            sys.stdout.flush()
        connection.close()
    if latency_report:
        print(latency.report())
    return 0


def parse_args(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Read the watch log without an open-time loader reset."
    )
    parser.add_argument("--port", required=True, help="Local watch serial port")
    parser.add_argument("--duration", type=float, default=10.0)
    parser.add_argument(
        "--latency-report",
        action="store_true",
        help="Print p50 and p90 for privacy-safe LATENCY fields.",
    )
    return parser.parse_args(arguments)


def main(arguments: list[str] | None = None) -> int:
    args = parse_args(arguments)
    if args.duration <= 0:
        print("The duration must be greater than zero.", file=sys.stderr)
        return 2
    return monitor(
        args.port, args.duration, latency_report=args.latency_report
    )


if __name__ == "__main__":
    raise SystemExit(main())
