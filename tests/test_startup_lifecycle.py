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

    def test_reliable_splash_precedes_capture_and_deferred_views(self) -> None:
        ui = (ROOT / "firmware" / "main" / "ui.cpp").read_text(
            encoding="utf-8"
        )
        hidden = ui[
            ui.index("bool start_hidden()") : ui.index("bool publish_startup(")
        ]
        publish = ui[
            ui.index("bool publish_startup(") : ui.index(
                "bool prepare_capture_views()"
            )
        ]
        capture = ui[
            ui.index("bool prepare_capture_views()") : ui.index(
                "bool prepare_visual_views()"
            )
        ]
        deferred = ui[
            ui.index("bool prepare_deferred_views(") : ui.index(
                "bool set_clock_style"
            )
        ]

        self.assertIn("create_startup_screen();", hidden)
        self.assertIn("lv_refr_now(display);", hidden)
        self.assertNotIn("bsp_display_brightness_set", hidden)
        self.assertEqual(
            publish.count("bsp_display_brightness_set(brightness_percent)"), 2
        )
        self.assertIn("lv_refr_now(display_handle);", publish)
        self.assertIn("create_runtime_screen();", capture)
        self.assertNotIn("bsp_display_start_touch", capture)
        self.assertIn("bsp_display_start_touch(display_handle)", deferred)
        self.assertIn("create_quick_controls(active_screen())", deferred)

    def test_preferences_load_during_hidden_panel_start(self) -> None:
        app = (ROOT / "firmware" / "main" / "app_main.cpp").read_text(
            encoding="utf-8"
        )
        main = app[app.index('extern "C" void app_main()') :]

        task_start = main.index("const bool preference_task_started = xTaskCreate(")
        hidden_start = main.index("chatesp::ui::start_hidden()")
        join = main.index("const esp_err_t preferences_result")
        publish = main.index("chatesp::ui::publish_startup(")
        self.assertLess(task_start, hidden_start)
        self.assertLess(hidden_start, join)
        self.assertLess(join, publish)
        self.assertIn("if (!task_started)", app)
        self.assertIn("return load.store->initialize();", app)
        self.assertIn("ulTaskNotifyTake(pdTRUE, portMAX_DELAY)", app)

    def test_startup_uses_confirmed_power_key_credit(self) -> None:
        app = (ROOT / "firmware" / "main" / "app_main.cpp").read_text(
            encoding="utf-8"
        )

        self.assertIn(
            "power::startup_button_started_at_ms(monotonic_ms())", app
        )
        for event in (
            "startup_panel_ready",
            "startup_first_pixel",
            "startup_capture_ready",
        ):
            self.assertIn(f"CrashEvent::{event}", app)
        self.assertLess(
            app.index("runtime.start("),
            app.index("CrashEvent::startup_capture_ready"),
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

    def test_saved_memories_do_not_block_capture_start(self) -> None:
        app = (ROOT / "firmware" / "main" / "app_main.cpp").read_text(
            encoding="utf-8"
        )

        self.assertNotIn("device_memory_store.initialize()", app)
        self.assertLess(
            app.index("static chatesp::DeviceMemoryStore device_memory_store"),
            app.index("runtime.start("),
        )

    def test_optional_views_are_deferred_and_idempotent(self) -> None:
        ui = (ROOT / "firmware" / "main" / "ui.cpp").read_text(
            encoding="utf-8"
        )
        capture_create = ui[
            ui.index("void create_runtime_screen()") : ui.index(
                "void create_visual_screen()"
            )
        ]
        visual_create = ui[
            ui.index("void create_visual_screen()") : ui.index(
                "}  // namespace", ui.index("void create_visual_screen()")
            )
        ]
        deferred_prepare = ui[
            ui.index("bool prepare_deferred_views(") : ui.index(
                "bool set_clock_style"
            )
        ]

        for optional_call in (
            "image_overlay = lv_image_create",
            "plot_overlay = lv_obj_create",
            "create_quick_controls",
            "create_clock_face",
        ):
            self.assertNotIn(optional_call, capture_create)
        self.assertIn("image_overlay = lv_image_create", visual_create)
        self.assertIn("plot_overlay = lv_obj_create", visual_create)
        self.assertIn("create_quick_controls(active_screen())", deferred_prepare)
        self.assertIn("create_clock_face(active_screen())", ui)
        self.assertIn("if (deferred_views_ready)", deferred_prepare)
        self.assertIn("deferred_views_ready = true;", deferred_prepare)

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
        startup = ui[
            ui.index("bool start_hidden()") : ui.index("bool set_clock_style(")
        ]
        layout = ui[
            ui.index("void layout_clock_path(") : ui.index(
                "ClockPathSpan current_clock_path_span("
            )
        ]

        self.assertIn("apply_clock_style(false);", create_clock)
        self.assertNotIn("layout_clock_path();", create_clock)
        self.assertIn("apply_clock_style(true);", show_mode)
        self.assertNotIn("heap_caps_calloc(", startup)
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
