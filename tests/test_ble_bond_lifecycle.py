from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "firmware" / "main" / "ble_provisioning.cpp"


class BleBondLifecycleTests(unittest.TestCase):
    def test_device_advertises_the_product_name(self) -> None:
        source = SOURCE.read_text(encoding="utf-8")

        self.assertIn('constexpr char kDeviceName[] = "ChatESP";', source)
        self.assertNotIn("ChatESP Setup", source)

    def test_volatile_bond_capture_does_not_use_the_stop_task_stack(self) -> None:
        source = SOURCE.read_text(encoding="utf-8")

        self.assertNotIn("VolatileStoreCapture capture;", source)
        self.assertIn("&s_volatile_store", source)

if __name__ == "__main__":
    unittest.main()
