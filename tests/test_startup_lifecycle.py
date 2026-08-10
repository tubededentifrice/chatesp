from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class StartupLifecycleTests(unittest.TestCase):
    def test_production_uses_bounded_boot_speed_settings(self) -> None:
        production = (
            ROOT / "firmware" / "config" / "watch_prod.defaults"
        ).read_text(encoding="utf-8")
        development = (
            ROOT / "firmware" / "config" / "watch_dev.defaults"
        ).read_text(encoding="utf-8")

        self.assertIn("CONFIG_SPIRAM_MEMTEST=n", production)
        self.assertIn(
            "CONFIG_BOOTLOADER_COMPILER_OPTIMIZATION_PERF=y", production
        )
        self.assertIn("CONFIG_BOOTLOADER_LOG_LEVEL_WARN=y", production)
        self.assertNotIn("CONFIG_SPIRAM_MEMTEST=n", development)
        self.assertNotIn("CONFIG_BOOTLOADER_LOG_LEVEL_WARN=y", development)

    def test_reliable_splash_precedes_hidden_runtime_views(self) -> None:
        ui = (ROOT / "firmware" / "main" / "ui.cpp").read_text(
            encoding="utf-8"
        )
        start = ui[
            ui.index("bool start(std::uint8_t brightness_percent)") : ui.index(
                "bool set_clock_style"
            )
        ]

        self.assertLess(
            start.index("create_startup_screen();"),
            start.index("bsp_display_brightness_set(brightness_percent)"),
        )
        self.assertLess(
            start.index("bsp_display_brightness_set(brightness_percent)"),
            start.index("create_runtime_screen();"),
        )
        self.assertLess(
            start.index("bsp_display_brightness_set(brightness_percent)"),
            start.index("bsp_display_start_touch(display)"),
        )
        self.assertLess(
            start.index("bsp_display_start_touch(display)"),
            start.index("create_runtime_screen();"),
        )
        self.assertIn(
            "touch_available = bsp_display_start_touch(display) == ESP_OK",
            start,
        )
        self.assertNotIn(
            "if (bsp_display_start_touch(display) != ESP_OK)", start
        )

    def test_touch_probe_does_not_delay_the_first_splash_frame(self) -> None:
        board = (
            ROOT
            / "firmware"
            / "components"
            / "chatesp_board"
            / "esp32_s3_touch_amoled_1_8.c"
        ).read_text(encoding="utf-8")
        display_start = board[
            board.index("lv_display_t *bsp_display_start_with_config(") :
            board.index("esp_err_t bsp_display_start_touch(")
        ]
        touch_start = board[
            board.index("esp_err_t bsp_display_start_touch(") :
            board.index("lv_indev_t *bsp_display_get_input_dev(")
        ]

        self.assertNotIn("bsp_display_indev_init", display_start)
        self.assertIn("bsp_display_indev_init", touch_start)

    def test_saved_memories_load_after_the_splash_is_visible(self) -> None:
        app = (ROOT / "firmware" / "main" / "app_main.cpp").read_text(
            encoding="utf-8"
        )

        self.assertLess(
            app.index("chatesp::ui::start("),
            app.index("device_memory_store.initialize()"),
        )

    def test_clock_path_layout_is_deferred_until_clock_opens(self) -> None:
        ui = (ROOT / "firmware" / "main" / "ui.cpp").read_text(
            encoding="utf-8"
        )
        create_clock = ui[
            ui.index("void create_clock_face(") : ui.index(
                "void bring_clock_overlays_forward("
            )
        ]
        show_mode = ui[
            ui.index("void show_app_mode(") : ui.index(
                "void show_clock_time("
            )
        ]
        start = ui[
            ui.index("bool start(") : ui.index("bool set_clock_style(")
        ]
        layout = ui[
            ui.index("void layout_clock_path(") : ui.index(
                "ClockPathSpan current_clock_path_span("
            )
        ]

        self.assertIn("apply_clock_style(false);", create_clock)
        self.assertNotIn("layout_clock_path();", create_clock)
        self.assertIn("apply_clock_style(true);", show_mode)
        self.assertNotIn("heap_caps_calloc(", start)
        self.assertIn("heap_caps_calloc(", layout)

    def test_development_marker_is_centered_and_build_guarded(self) -> None:
        ui = (ROOT / "firmware" / "main" / "ui.cpp").read_text(
            encoding="utf-8"
        )
        runtime_start = ui.index("void create_runtime_screen(")
        marker_start = ui.index(
            "#if CHATESP_DEVELOPMENT_MODE\n"
            "    development_status_label",
            runtime_start,
        )
        marker_end = ui.index("    show_footer(", marker_start)
        marker = ui[marker_start:marker_end]

        self.assertIn('set_static_text(development_status_label, "DEV");', marker)
        self.assertIn("LV_ALIGN_BOTTOM_MID, 0, -20", marker)
        self.assertIn("&lv_font_montserrat_14", marker)
        self.assertIn("#endif", marker)


if __name__ == "__main__":
    unittest.main()
