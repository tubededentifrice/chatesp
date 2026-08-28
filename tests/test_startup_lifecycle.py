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
            ui.index("bool start_hidden(") : ui.index("bool publish_startup(")
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

    def test_preferences_load_after_hidden_panel_start(self) -> None:
        app = (ROOT / "firmware" / "main" / "app_main.cpp").read_text(
            encoding="utf-8"
        )
        main = app[app.index('extern "C" void app_main()') :]

        hidden_start = main.index("chatesp::ui::start_hidden(")
        preferences = main.index("device_preferences_store.initialize()")
        publish = main.index("chatesp::ui::publish_startup(")
        self.assertLess(hidden_start, preferences)
        self.assertLess(preferences, publish)
        self.assertNotIn("preference_load_task", app)
        self.assertNotIn('"device_preferences"', app)

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

    def test_revision_probe_precedes_panel_and_touch_input_stays_deferred(
        self,
    ) -> None:
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
        revision_probe = board[
            board.index("bsp_board_revision_t bsp_board_revision(") :
            board.index("esp_err_t bsp_board_diagnostics_get(")
        ]
        display_new = board[
            board.index("esp_err_t bsp_display_new(") :
            board.index("esp_err_t bsp_touch_new(")
        ]
        touch_new = board[
            board.index("esp_err_t bsp_touch_new(") :
            board.index("static esp_err_t bsp_io_expander_init(")
        ]

        self.assertNotIn("bsp_display_indev_init", display_start)
        self.assertIn("bsp_display_indev_init", touch_start)
        self.assertLess(
            revision_probe.index("bsp_i2c_device_probe(0x15)"),
            revision_probe.index("bsp_i2c_device_probe(0x38)"),
        )
        self.assertLess(
            display_new.index("bsp_board_revision()"),
            display_new.index("esp_lcd_new_panel_"),
        )
        self.assertNotIn("bsp_i2c_device_probe", touch_new)
        self.assertLess(
            display_start.index("lvgl_port_lock(0)"),
            display_start.index("bsp_display_lcd_init(cfg)"),
        )
        self.assertLess(
            display_start.index("bsp_display_brightness_init()"),
            display_start.index("lvgl_port_unlock()"),
        )

    def test_unknown_revision_uses_v2_display_without_touch(self) -> None:
        board = (
            ROOT
            / "firmware"
            / "components"
            / "chatesp_board"
            / "esp32_s3_touch_amoled_1_8.c"
        ).read_text(encoding="utf-8")
        display_new = board[
            board.index("esp_err_t bsp_display_new(") :
            board.index("esp_err_t bsp_touch_new(")
        ]
        touch_new = board[
            board.index("esp_err_t bsp_touch_new(") :
            board.index("static esp_err_t bsp_io_expander_init(")
        ]

        self.assertIn("esp_lcd_new_panel_co5300", display_new)
        self.assertIn("BSP_LCD_CST816S_X_GAP", display_new)
        self.assertIn(
            "revision == BSP_BOARD_REVISION_UNKNOWN", touch_new
        )
        self.assertIn("return ESP_ERR_NOT_FOUND", touch_new)
        self.assertNotIn("bsp_i2c_device_probe", touch_new)

    def test_original_revision_selects_sh8601_and_framed_brightness(
        self,
    ) -> None:
        board = (
            ROOT
            / "firmware"
            / "components"
            / "chatesp_board"
            / "esp32_s3_touch_amoled_1_8.c"
        ).read_text(encoding="utf-8")
        manifest = (
            ROOT
            / "firmware"
            / "components"
            / "chatesp_board"
            / "idf_component.yml"
        ).read_text(encoding="utf-8")
        display_new = board[
            board.index("esp_err_t bsp_display_new(") :
            board.index("esp_err_t bsp_touch_new(")
        ]

        self.assertIn('waveshare/esp_lcd_sh8601: "2.0.0"', manifest)
        self.assertIn("SH8601_PANEL_BUS_QSPI_CONFIG", display_new)
        self.assertIn("esp_lcd_new_panel_sh8601", display_new)
        self.assertIn("BSP_SH8601_QSPI_WRITE_CMD(0x51)", board)

    def test_touch_reader_is_local_single_point_and_nonfatal(self) -> None:
        board = (
            ROOT
            / "firmware"
            / "components"
            / "chatesp_board"
            / "esp32_s3_touch_amoled_1_8.c"
        ).read_text(encoding="utf-8")
        callback = board[
            board.index("static void bsp_touchpad_read(") :
            board.index("static lv_indev_t *bsp_display_indev_init(")
        ]
        indev_start = board.index(
            "static lv_indev_t *bsp_display_indev_init("
        )
        indev_init = board[
            indev_start : board.index("/*****", indev_start)
        ]

        self.assertNotIn("lvgl_port_add_touch", board)
        self.assertIn("data->state = LV_INDEV_STATE_RELEASED", callback)
        self.assertIn("esp_lcd_touch_read_data", callback)
        self.assertIn("esp_lcd_touch_get_data", callback)
        self.assertIn("touch, &point, &point_count, 1", callback)
        self.assertNotIn("ESP_ERROR_CHECK", callback)
        self.assertNotIn("point.x", callback.split("ESP_LOG", 1)[0])
        self.assertNotIn("BSP_ERROR_CHECK_RETURN_NULL", indev_init)
        self.assertIn("touch_result != ESP_OK", indev_init)

    def test_cold_reset_and_power_input_use_disjoint_expander_masks(
        self,
    ) -> None:
        board = (
            ROOT
            / "firmware"
            / "components"
            / "chatesp_board"
            / "esp32_s3_touch_amoled_1_8.c"
        ).read_text(encoding="utf-8")
        reset = board[
            board.index("static esp_err_t bsp_cold_reset_outputs(void)\n{") :
            board.index("esp_err_t bsp_power_button_init(")
        ]

        self.assertIn("BSP_EXIO_COLD_RESET_MASK", reset)
        self.assertIn("vTaskDelay(pdMS_TO_TICKS(20))", reset)
        self.assertIn("BSP_IO_EXPANDER_OUTPUT_REG", reset)
        self.assertIn("BSP_IO_EXPANDER_DIRECTION_REG", reset)
        self.assertIn("BSP_EXIO_COLD_RESET_MASK, false", reset)
        self.assertIn("BSP_EXIO_COLD_RESET_MASK, true", reset)
        self.assertNotIn("BSP_EXIO_POWER_BUTTON", reset)
        self.assertIn(
            "BSP_EXIO_POWER_BUTTON, true", board
        )
        self.assertNotIn("esp_io_expander_new_i2c_tca9554", board)
        self.assertIn("bsp_io_expander_read_reg(reg, &current)", board)

    def test_runtime_brightness_uses_display_controller_safe_point(
        self,
    ) -> None:
        runtime = (
            ROOT / "firmware" / "main" / "voice_runtime.cpp"
        ).read_text(encoding="utf-8")
        ui = (ROOT / "firmware" / "main" / "ui.cpp").read_text(
            encoding="utf-8"
        )
        controller = runtime[
            runtime.index("void run_display_controller()") :
            runtime.index("bool display_request_complete(")
        ]
        brightness = ui[
            ui.index("esp_err_t set_brightness(") :
            ui.index("}  // namespace chatesp::ui")
        ]

        self.assertIn("perform_brightness_request", controller)
        self.assertIn("brightness_coordinator_.begin_next()", controller)
        self.assertIn("brightness_work.valid()", controller)
        self.assertLess(
            brightness.index("bsp_display_lock(1000)"),
            brightness.index("lv_refr_now(display_handle)"),
        )
        self.assertLess(
            brightness.index("lv_refr_now(display_handle)"),
            brightness.index("bsp_display_brightness_set"),
        )
        self.assertLess(
            brightness.index("bsp_display_brightness_set"),
            brightness.index("bsp_display_unlock()"),
        )
        wake = ui[
            ui.index("esp_err_t wake(") : ui.index("esp_err_t set_brightness(")
        ]
        self.assertLess(
            wake.index("lv_refr_now(display_handle)"),
            wake.index("bsp_display_recover()"),
        )
        request = runtime[
            runtime.index("std::uint32_t request_display(") :
            runtime.index("void perform_display_request(")
        ]
        self.assertNotIn("perform_display_request(generation);", request)
        submit = runtime[
            runtime.index("runtime::BrightnessSubmission submit_brightness(") :
            runtime.index("runtime::BrightnessOutcome brightness_outcome(")
        ]
        self.assertNotIn("perform_brightness_request", submit)

    def test_touch_brightness_is_async_and_model_brightness_waits(self) -> None:
        runtime = (
            ROOT / "firmware" / "main" / "voice_runtime.cpp"
        ).read_text(encoding="utf-8")
        device = (
            ROOT / "firmware" / "main" / "device_control.cpp"
        ).read_text(encoding="utf-8")
        quick_controls = runtime[
            runtime.index("void process_quick_controls(") :
            runtime.index("void run()")
        ]
        request = runtime[
            runtime.index("agent::Error request_brightness(") :
            runtime.index("void perform_brightness_request(")
        ]
        model_set = device[
            device.index("agent::Error DeviceControl::set_brightness(") :
            device.index("agent::Error DeviceControl::preview_brightness(")
        ]

        self.assertIn("submit_brightness(brightness)", quick_controls)
        self.assertIn("finish_quick_controls_commit", quick_controls)
        self.assertIn("quick_controls_commit_revision_active_", runtime)
        self.assertIn("brightness_outcome", runtime)
        self.assertIn("persist_preferences({", runtime)
        self.assertIn("quick_controls_commit_volume_active_", runtime)
        self.assertIn("quick_controls_snapshot_lock_", runtime)
        self.assertNotIn("bsp_display_lock", quick_controls)
        self.assertIn("if (!wait)", request)
        self.assertLess(
            request.index("if (!wait)"),
            request.index("while (true)"),
        )
        self.assertIn("cancel_brightness", request)
        self.assertIn("remember_brightness_reconciliation", request)
        self.assertIn("preview_brightness(percent)", model_set)
        self.assertNotIn("ui::set_brightness", device)
        preview = device[
            device.index("agent::Error DeviceControl::preview_brightness(") :
            device.index("void DeviceControl::confirm_brightness(")
        ]
        self.assertLess(
            preview.index("if (!wait)"),
            preview.index("brightness_percent_.store"),
        )

        sleep = runtime[
            runtime.index("void enter_sleep()") :
            runtime.index("bool stop_ble_for_request()")
        ]
        self.assertLess(
            sleep.index("cancel_pending_brightness()"),
            sleep.index("request_display("),
        )

    def test_touch_calibration_is_development_only_and_memory_only(self) -> None:
        ui = (ROOT / "firmware" / "main" / "ui.cpp").read_text(
            encoding="utf-8"
        )
        calibration_start = ui.index(
            "#if CHATESP_DEVELOPMENT_MODE\n"
            "void layout_touch_calibration_target()"
        )
        calibration_end = ui.index("#endif", calibration_start)
        calibration = ui[calibration_start:calibration_end]

        self.assertIn("touch_calibration_event", calibration)
        self.assertIn("lv_event_get_indev(event)", calibration)
        self.assertIn("DX %+ld  DY %+ld", calibration)
        self.assertIn("LV_DISPLAY_ROTATION_270", calibration)
        self.assertIn("apply_display_orientation();", calibration)
        self.assertNotIn("ESP_LOG", calibration)
        self.assertNotIn("nvs_", calibration.lower())

    def test_lvgl_watermark_timer_does_not_depend_on_touch(self) -> None:
        ui = (ROOT / "firmware" / "main" / "ui.cpp").read_text(
            encoding="utf-8"
        )
        callback = ui[
            ui.index("void resource_telemetry_timer_callback(") :
            ui.index("void controls_timer_callback(")
        ]
        startup = ui[
            ui.index("void create_runtime_screen(") :
            ui.index("void create_visual_screen(")
        ]
        controls = ui[
            ui.index("void controls_timer_callback(") :
            ui.index("void set_controls_state_allowed(")
        ]

        self.assertIn("TaskId::lvgl", callback)
        self.assertIn("resource_telemetry_timer = lv_timer_create", startup)
        self.assertNotIn("TaskId::lvgl", controls)

    def test_first_party_display_source_never_configures_gpio13(self) -> None:
        paths = []
        for source_root in (
            ROOT / "firmware" / "main",
            ROOT / "firmware" / "lib",
            ROOT / "firmware" / "components",
        ):
            paths.extend(
                path for path in source_root.rglob("*")
                if path.is_file() and path.suffix in {
                    ".c", ".cc", ".cpp", ".h", ".hpp"
                }
            )
        for path in paths:
            with self.subTest(path=path):
                self.assertNotIn(
                    "GPIO_NUM_13", path.read_text(encoding="utf-8")
                )

    def test_only_board_adapter_owns_expander_pins(self) -> None:
        board_root = (
            ROOT / "firmware" / "components" / "chatesp_board"
        ).resolve()
        source_roots = [
            ROOT / "firmware" / "main",
            ROOT / "firmware" / "lib",
            ROOT / "firmware" / "components",
        ]
        for source_root in source_roots:
            for path in source_root.rglob("*"):
                if not path.is_file() or path.suffix not in {
                    ".c", ".cc", ".cpp", ".h", ".hpp"
                }:
                    continue
                if path.resolve().is_relative_to(board_root):
                    continue
                text = path.read_text(encoding="utf-8")
                with self.subTest(path=path):
                    self.assertNotIn("IO_EXPANDER_PIN_NUM_", text)
                    self.assertNotIn("esp_io_expander_", text)
                    self.assertNotIn("BSP_EXIO_", text)

        board_header = (
            board_root
            / "include"
            / "bsp"
            / "esp32_s3_touch_amoled_1_8.h"
        ).read_text(encoding="utf-8")
        self.assertNotIn("bsp_io_expander_init", board_header)

    def test_low_level_panel_commands_stay_in_display_owners(self) -> None:
        board_root = (
            ROOT / "firmware" / "components" / "chatesp_board"
        ).resolve()
        ui_owner = (ROOT / "firmware" / "main" / "ui.cpp").resolve()
        low_level_symbols = (
            "bsp_display_brightness_set(",
            "bsp_display_backlight_off(",
            "bsp_display_backlight_on(",
            "bsp_display_recover(",
            "esp_lcd_panel_disp_on_off(",
            "esp_lcd_panel_init(",
        )
        for source_root in (
            ROOT / "firmware" / "main",
            ROOT / "firmware" / "lib",
            ROOT / "firmware" / "components",
        ):
            for path in source_root.rglob("*"):
                if not path.is_file() or path.suffix not in {
                    ".c", ".cc", ".cpp", ".h", ".hpp"
                }:
                    continue
                if (
                    path.resolve().is_relative_to(board_root)
                    or path.resolve() == ui_owner
                ):
                    continue
                text = path.read_text(encoding="utf-8")
                with self.subTest(path=path):
                    for symbol in low_level_symbols:
                        self.assertNotIn(symbol, text)

    def test_saved_memories_are_not_loaded_on_main_task(self) -> None:
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
            ui.index("bool start_hidden(") : ui.index("bool set_clock_style(")
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

    def test_clock_path_draws_last_and_invalidates_only_its_change(self) -> None:
        ui = (ROOT / "firmware" / "main" / "ui.cpp").read_text(
            encoding="utf-8"
        )
        timer_start = ui.index("void clock_path_timer_callback(")
        timer = ui[timer_start : ui.index("void draw_clock(", timer_start)]
        draw_start = ui.index("void draw_clock(")
        draw = ui[draw_start : ui.index("void apply_clock_style(", draw_start)]
        create_clock = ui[
            ui.index("void create_clock_face(") : ui.index(
                "void bring_clock_overlays_forward("
            )
        ]

        self.assertIn("invalidate_clock_path_change(previous, next);", timer)
        self.assertNotIn("lv_obj_invalidate(clock_root);", timer)
        self.assertIn("LV_EVENT_DRAW_POST", draw)
        self.assertIn("LV_EVENT_DRAW_POST", create_clock)

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
