from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class UiContractTests(unittest.TestCase):
    def test_charge_mark_is_overlaid_on_the_battery_icon(self) -> None:
        ui = (ROOT / "firmware" / "main" / "ui.cpp").read_text(
            encoding="utf-8"
        )
        create_start = ui.index("void create_runtime_screen(")
        create_end = ui.index("}  // namespace", create_start)
        create_source = ui[create_start:create_end]
        footer_start = ui.index("void show_footer(")
        footer_end = ui.index("bool show_fullscreen_image(", footer_start)
        footer_source = ui[footer_start:footer_end]

        self.assertIn(
            "set_static_text(battery_charge_label, LV_SYMBOL_CHARGE)",
            create_source,
        )
        self.assertIn(
            "battery_charge_label, battery_icon_label, LV_ALIGN_CENTER",
            create_source,
        )
        self.assertIn(
            "lv_obj_set_style_transform_scale(\n"
            "        battery_charge_label",
            create_source,
        )
        self.assertIn(
            "set_hidden(battery_charge_label, !external_power_connected)",
            footer_source,
        )
        self.assertIn(
            "set_static_text(battery_icon_label, battery_icon)",
            footer_source,
        )
        self.assertNotIn('"%s" LV_SYMBOL_CHARGE', footer_source)


if __name__ == "__main__":
    unittest.main()
