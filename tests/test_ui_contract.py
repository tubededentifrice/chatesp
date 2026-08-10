from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class UiContractTests(unittest.TestCase):
    def test_control_handle_follows_the_panel_bottom(self) -> None:
        ui = (ROOT / "firmware" / "main" / "ui.cpp").read_text(
            encoding="utf-8"
        )
        constants_end = ui.index("enum class ControlKind")
        constants = ui[:constants_end]
        create_start = ui.index("void create_quick_controls(")
        create_end = ui.index("void create_startup_screen()", create_start)
        create_source = ui[create_start:create_end]

        self.assertIn(
            "kControlsPanelHandleY =\n"
            "    kControlsPanelHeight - kControlsPanelHandleBottomInset -\n"
            "    kControlsHandleHeight",
            constants,
        )
        self.assertIn(
            "kControlsPanelHiddenY =\n"
            "    kControlsEdgeHandleY - kControlsPanelHandleY",
            constants,
        )
        self.assertIn(
            "panel_handle, LV_ALIGN_TOP_MID, 0, kControlsPanelHandleY",
            create_source,
        )
        self.assertIn(
            "controls_edge_handle, LV_ALIGN_TOP_MID, 0, "
            "kControlsEdgeHandleY",
            create_source,
        )

    def test_long_text_uses_direct_momentum_scrolling(self) -> None:
        ui = (ROOT / "firmware" / "main" / "ui.cpp").read_text(
            encoding="utf-8"
        )
        create_start = ui.index("void create_runtime_screen()")
        create_end = ui.index("recording_spectrum::create(screen)", create_start)
        create_source = ui[create_start:create_end]

        self.assertIn("content_viewport = lv_obj_create(screen)", create_source)
        self.assertIn(
            "lv_obj_set_scroll_dir(content_viewport, LV_DIR_VER)",
            create_source,
        )
        self.assertIn(
            "lv_obj_set_scrollbar_mode(content_viewport, "
            "LV_SCROLLBAR_MODE_ACTIVE)",
            create_source,
        )
        self.assertIn("LV_OBJ_FLAG_SCROLL_ELASTIC", create_source)
        self.assertIn("LV_OBJ_FLAG_SCROLL_MOMENTUM", create_source)
        self.assertIn("LV_OBJ_FLAG_PRESS_LOCK", create_source)
        self.assertIn(
            "lv_obj_clear_flag(content_viewport, LV_OBJ_FLAG_SCROLL_CHAIN)",
            create_source,
        )
        self.assertIn(
            "content_label = lv_label_create(content_viewport)",
            create_source,
        )
        self.assertIn(
            "lv_obj_set_height(content_label, LV_SIZE_CONTENT)",
            create_source,
        )

    def test_stream_updates_keep_the_answer_scroll_position(self) -> None:
        ui = (ROOT / "firmware" / "main" / "ui.cpp").read_text(
            encoding="utf-8"
        )
        set_start = ui.index("void set_content(")
        set_end = ui.index("void set_hint(", set_start)
        set_source = ui[set_start:set_end]

        self.assertIn(
            "const bool reset_scroll = shown_content_kind != kind",
            set_source,
        )
        self.assertIn(
            "lv_obj_scroll_to_y(content_viewport, 0, LV_ANIM_OFF)",
            set_source,
        )
        self.assertIn("stop_content_scroll()", set_source)
        self.assertIn(
            "set_content(answer, kMaximumAnswerBytes, ContentKind::answer)",
            ui,
        )

        stop_start = ui.index("void stop_content_scroll()")
        stop_end = ui.index("void set_content(", stop_start)
        stop_source = ui[stop_start:stop_end]
        self.assertIn(
            "lv_indev_get_scroll_obj(input) == content_viewport",
            stop_source,
        )
        self.assertIn(
            "lv_indev_reset(input, content_viewport)",
            stop_source,
        )

    def test_clock_blocks_touches_from_reaching_answer_text(self) -> None:
        ui = (ROOT / "firmware" / "main" / "ui.cpp").read_text(
            encoding="utf-8"
        )
        mode_start = ui.index("void show_app_mode(")
        mode_end = ui.index("void show_clock_time(", mode_start)
        mode_source = ui[mode_start:mode_end]

        self.assertIn("set_hidden(content_viewport, true)", mode_source)
        self.assertIn("set_hidden(content_viewport, false)", mode_source)

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
