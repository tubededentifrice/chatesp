from __future__ import annotations

import unittest
from tools.watch_monitor import open_safe_serial


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

if __name__ == "__main__":
    unittest.main()
