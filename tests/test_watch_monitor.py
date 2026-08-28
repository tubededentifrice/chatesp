from __future__ import annotations

import unittest
from io import StringIO
from unittest.mock import patch

import serial
from tools.watch_monitor import (
    LatencySummary,
    SerialRedactor,
    close_safe_serial,
    disable_hangup_reset,
    monitor,
    main,
    open_safe_serial,
    reopen_safe_serial,
    redact_serial_text,
)


class RecordingSerial:
    def __init__(self) -> None:
        object.__setattr__(self, "events", [])

    def __setattr__(self, name: str, value: object) -> None:
        self.events.append((name, value))
        object.__setattr__(self, name, value)

    def open(self) -> None:
        self.events.append(("open", None))

    def setDTR(self, value: bool) -> None:
        self.events.append(("setDTR", value))

    def setRTS(self, value: bool) -> None:
        self.events.append(("setRTS", value))

    def close(self) -> None:
        self.events.append(("close", None))

    def fileno(self) -> int:
        return 7


class DisconnectingSerial:
    def __init__(self, lines: list[bytes]) -> None:
        self.lines = lines
        self.closed = False

    @property
    def in_waiting(self) -> int:
        return 1

    def read(self, _: int) -> bytes:
        if self.lines:
            return self.lines.pop(0)
        raise serial.SerialException("device disconnected")

    def close(self) -> None:
        self.closed = True


class RecordingTermios:
    HUPCL = 0x4000
    TCSANOW = 0

    def __init__(self) -> None:
        self.events: list[tuple[object, ...]] = []

    def tcgetattr(self, file_descriptor: int) -> list[int]:
        self.events.append(("get", file_descriptor))
        return [0, 0, self.HUPCL | 0x20, 0, 0, 0, 0]

    def tcsetattr(
        self, file_descriptor: int, when: int, attributes: list[int]
    ) -> None:
        self.events.append(("set", file_descriptor, when, attributes[2]))


class WatchMonitorTests(unittest.TestCase):
    def test_control_lines_are_inactive_before_open(self) -> None:
        connection = open_safe_serial(
            "LOCAL_PORT",
            RecordingSerial,
            lambda value: value.events.append(("disable_hupcl", None)),
        )

        self.assertLess(
            connection.events.index(("dtr", False)),
            connection.events.index(("open", None)),
        )
        self.assertLess(
            connection.events.index(("rts", False)),
            connection.events.index(("open", None)),
        )
        self.assertGreater(
            connection.events.index(("disable_hupcl", None)),
            connection.events.index(("open", None)),
        )

    def test_open_disables_close_time_hangup_reset(self) -> None:
        connection = RecordingSerial()
        terminal = RecordingTermios()

        disable_hangup_reset(connection, terminal)

        self.assertEqual(
            terminal.events,
            [("get", 7), ("set", 7, terminal.TCSANOW, 0x20)],
        )

    def test_same_port_reopen_is_bounded_until_the_port_returns(self) -> None:
        connection = RecordingSerial()
        attempts: list[str] = []
        clock = iter((0.0, 0.0, 0.1, 0.2, 0.3, 0.4))

        def opener(port: str) -> RecordingSerial:
            attempts.append(port)
            if len(attempts) < 3:
                raise OSError(2, "No such file")
            return connection

        waits: list[float] = []
        result = reopen_safe_serial(
            "LOCAL_PORT",
            timeout_seconds=1.0,
            interval_seconds=0.1,
            opener=opener,
            monotonic=lambda: next(clock),
            wait=waits.append,
        )

        self.assertIs(connection, result)
        self.assertEqual(["LOCAL_PORT"] * 3, attempts)
        self.assertEqual([0.1, 0.1], waits)

    def test_same_port_reopen_stops_after_the_fixed_timeout(self) -> None:
        attempts: list[str] = []

        def opener(port: str) -> RecordingSerial:
            attempts.append(port)
            raise serial.SerialException("could not open port: No such file")

        clock = iter((0.0, 0.0, 0.5))
        with self.assertRaises(serial.SerialException):
            reopen_safe_serial(
                "LOCAL_PORT",
                timeout_seconds=0.5,
                interval_seconds=0.1,
                opener=opener,
                monotonic=lambda: next(clock),
                wait=lambda _: None,
            )

        self.assertEqual(["LOCAL_PORT"], attempts)

    def test_same_port_reopen_does_not_retry_permission_failure(self) -> None:
        attempts: list[str] = []

        def opener(port: str) -> RecordingSerial:
            attempts.append(port)
            raise serial.SerialException("could not open port: Permission denied")

        with self.assertRaises(serial.SerialException):
            reopen_safe_serial(
                "LOCAL_PORT",
                opener=opener,
                monotonic=lambda: 0.0,
                wait=lambda _: None,
            )

        self.assertEqual(["LOCAL_PORT"], attempts)

    def test_same_port_reopen_does_not_retry_invalid_argument(self) -> None:
        attempts: list[str] = []

        def opener(port: str) -> RecordingSerial:
            attempts.append(port)
            raise serial.SerialException(
                "could not open port: Invalid argument"
            )

        with self.assertRaises(serial.SerialException):
            reopen_safe_serial(
                "LOCAL_PORT",
                opener=opener,
                monotonic=lambda: 0.0,
                wait=lambda _: None,
            )

        self.assertEqual(["LOCAL_PORT"], attempts)

    def test_close_resets_native_usb_into_application_mode(self) -> None:
        connection = RecordingSerial()

        close_safe_serial(
            connection,
            lambda duration: connection.events.append(("wait", duration)),
        )

        self.assertEqual(
            connection.events,
            [
                ("setDTR", False),
                ("setRTS", True),
                ("wait", 0.15),
                ("setRTS", False),
                ("wait", 0.5),
                ("setDTR", False),
                ("setRTS", False),
                ("close", None),
            ],
        )

    def test_production_system_off_disconnect_is_a_clean_monitor_end(self) -> None:
        connection = DisconnectingSerial(
            [b"I power: Requesting AXP2101 system off\r\n"]
        )

        with patch(
            "tools.watch_monitor.open_safe_serial", return_value=connection
        ), patch("tools.watch_monitor.close_safe_serial") as safe_close, patch(
            "tools.watch_monitor.time.monotonic", side_effect=[0.0, 0.0, 0.5]
        ), patch("tools.watch_monitor.sys.stdout", new_callable=StringIO):
            result = monitor("LOCAL_PORT", 1.0)

        self.assertEqual(result, 0)
        self.assertTrue(connection.closed)
        safe_close.assert_not_called()

    def test_unexpected_disconnect_is_still_an_error(self) -> None:
        connection = DisconnectingSerial([])

        with patch(
            "tools.watch_monitor.open_safe_serial", return_value=connection
        ), patch("tools.watch_monitor.close_safe_serial") as safe_close, patch(
            "tools.watch_monitor.time.monotonic", side_effect=[0.0, 0.0]
        ):
            with self.assertRaises(serial.SerialException):
                monitor("LOCAL_PORT", 1.0)

        safe_close.assert_called_once_with(connection)

    def test_cli_redacts_an_unexpected_serial_error(self) -> None:
        with patch(
            "tools.watch_monitor.monitor",
            side_effect=serial.SerialException(
                "private raw port error /dev/cu.private"
            ),
        ), patch("tools.watch_monitor.sys.stderr", new_callable=StringIO) as error:
            result = main(["--port", "/dev/cu.private", "--duration", "1"])

        self.assertEqual(1, result)
        self.assertNotIn("/dev/cu.private", error.getvalue())
        self.assertNotIn("private raw", error.getvalue())

    def test_system_off_disconnect_during_close_is_clean(self) -> None:
        connection = DisconnectingSerial(
            [b"I power: Requesting AXP2101 system off\r\n"]
        )

        with patch(
            "tools.watch_monitor.open_safe_serial", return_value=connection
        ), patch(
            "tools.watch_monitor.close_safe_serial",
            side_effect=serial.SerialException("device disconnected"),
        ) as safe_close, patch(
            "tools.watch_monitor.time.monotonic", side_effect=[0.0, 0.0, 2.0]
        ), patch("tools.watch_monitor.sys.stdout", new_callable=StringIO):
            result = monitor("LOCAL_PORT", 1.0)

        self.assertEqual(result, 0)
        safe_close.assert_called_once_with(connection)

    def test_local_network_identifiers_are_redacted(self) -> None:
        text = (
            "wifi:connected with private-name, aid = 2, "
            "bssid = 12:34:56:78:9a:bc\n"
            "sta ip: 192.168.15.47, mask: 255.255.255.0, "
            "gw: 192.168.15.1\n"
            "sta ip6: fe80::1234\n"
            "ssid: another-private-name\n"
        )

        redacted = redact_serial_text(text)

        self.assertNotIn("private-name", redacted)
        self.assertNotIn("12:34:56:78:9a:bc", redacted)
        self.assertNotIn("192.168.15.47", redacted)
        self.assertNotIn("255.255.255.0", redacted)
        self.assertNotIn("192.168.15.1", redacted)
        self.assertNotIn("fe80::1234", redacted)
        self.assertNotIn("another-private-name", redacted)
        self.assertIn("[redacted network]", redacted)
        self.assertEqual(redacted.count("[redacted address]"), 4)

    def test_network_names_with_commas_are_fully_redacted(self) -> None:
        text = (
            "wifi:connected with office,private, aid = 2\n"
            "ssid: home,private, channel = 6\n"
        )

        redacted = redact_serial_text(text)

        self.assertNotIn("office", redacted)
        self.assertNotIn("private", redacted)
        self.assertNotIn("home", redacted)
        self.assertNotIn("channel", redacted)
        self.assertEqual(redacted.count("[redacted network]"), 2)

    def test_overlong_line_is_discarded_until_the_next_line(self) -> None:
        redactor = SerialRedactor()

        first = redactor.feed("x" * 4_097 + "private-tail")
        second = redactor.feed("\nnext ip: 10.0.0.1\n")

        self.assertEqual(first, "[redacted overlong serial line]\n")
        self.assertNotIn("private-tail", first + second)
        self.assertEqual(second, "next ip: [redacted address]\n")
        self.assertEqual(redactor.finish(), "")

    def test_latency_summary_calculates_p50_and_p90(self) -> None:
        summary = LatencySummary()
        for value in range(1, 11):
            summary.add_line(
                f"I voice_runtime: LATENCY first_audio_ms={value * 100} "
                f"turn_ms={value * 200}"
            )
        summary.add_line("private answer text must not be parsed")

        report = summary.report()

        self.assertIn("first_audio_ms: n=10 p50=500 p90=900", report)
        self.assertIn("turn_ms: n=10 p50=1000 p90=1800", report)
        self.assertNotIn("private answer", report)

if __name__ == "__main__":
    unittest.main()
