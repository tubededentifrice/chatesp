from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class ClockOrientationTests(unittest.TestCase):
    def test_clock_uses_opposite_landscape_rotation(self) -> None:
        ui = (ROOT / "firmware" / "main" / "ui.cpp").read_text(
            encoding="utf-8"
        )
        start = ui.index("void apply_display_orientation()")
        end = ui.index("void rounded_clock_point", start)
        orientation = ui[start:end]

        self.assertIn("LV_DISPLAY_ROTATION_270", orientation)
        self.assertIn("LV_DISP_ROT_270", orientation)
        self.assertNotIn("LV_DISPLAY_ROTATION_90", orientation)
        self.assertNotIn("LV_DISP_ROT_90", orientation)


if __name__ == "__main__":
    unittest.main()
