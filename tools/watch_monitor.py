#!/usr/bin/env python3
"""Read the ESP32-S3 USB log without an open-time loader reset."""

from __future__ import annotations

import argparse
import sys
import time
from collections.abc import Callable

import serial


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


def monitor(port: str, duration_seconds: float) -> int:
    connection = open_safe_serial(port)
    try:
        deadline = time.monotonic() + duration_seconds
        while time.monotonic() < deadline:
            data = connection.read(connection.in_waiting or 1)
            if data:
                sys.stdout.write(data.decode("utf-8", errors="replace"))
                sys.stdout.flush()
    finally:
        connection.close()
    return 0


def parse_args(arguments: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Read the watch log without an open-time loader reset."
    )
    parser.add_argument("--port", required=True, help="Local watch serial port")
    parser.add_argument("--duration", type=float, default=10.0)
    return parser.parse_args(arguments)


def main(arguments: list[str] | None = None) -> int:
    args = parse_args(arguments)
    if args.duration <= 0:
        print("The duration must be greater than zero.", file=sys.stderr)
        return 2
    return monitor(args.port, args.duration)


if __name__ == "__main__":
    raise SystemExit(main())
