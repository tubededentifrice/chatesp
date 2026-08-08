from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class BleStopStackTests(unittest.TestCase):
    def test_bond_capture_uses_bounded_static_storage(self) -> None:
        source = (
            ROOT / "firmware" / "main" / "ble_provisioning.cpp"
        ).read_text(encoding="utf-8")

        capture = source[
            source.index("bool capture_volatile_store()") : source.index(
                "bool restore_volatile_store()"
            )
        ]
        self.assertNotIn("VolatileStoreCapture capture;", capture)
        self.assertNotIn("s_volatile_store = {};", capture)
        self.assertIn("s_volatile_store.count = 0;", capture)
        self.assertIn("&s_volatile_store", capture)


if __name__ == "__main__":
    unittest.main()
