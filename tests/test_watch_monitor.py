from __future__ import annotations

import unittest
from tools.watch_monitor import (
    SerialRedactor,
    open_safe_serial,
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


class WatchMonitorTests(unittest.TestCase):
    def test_control_lines_are_inactive_before_open(self) -> None:
        connection = open_safe_serial("LOCAL_PORT", RecordingSerial)

        self.assertLess(
            connection.events.index(("dtr", False)),
            connection.events.index(("open", None)),
        )
        self.assertLess(
            connection.events.index(("rts", False)),
            connection.events.index(("open", None)),
        )

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
        self.assertEqual(redacted.count("[redacted address]"), 5)

    def test_overlong_line_is_discarded_until_the_next_line(self) -> None:
        redactor = SerialRedactor()

        first = redactor.feed("x" * 4_097 + "private-tail")
        second = redactor.feed("\nnext ip: 10.0.0.1\n")

        self.assertEqual(first, "[redacted overlong serial line]\n")
        self.assertNotIn("private-tail", first + second)
        self.assertEqual(second, "next ip: [redacted address]\n")
        self.assertEqual(redactor.finish(), "")

if __name__ == "__main__":
    unittest.main()
