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
        self.assertNotIn("CONFIG_SPIRAM_MEMTEST=n", development)

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
        allocation_failure = start[
            start.index("if (clock_path_points == nullptr)") : start.index(
                "create_runtime_screen();"
            )
        ]
        self.assertIn("(void)bsp_display_backlight_off();", allocation_failure)

    def test_saved_memories_load_after_the_splash_is_visible(self) -> None:
        app = (ROOT / "firmware" / "main" / "app_main.cpp").read_text(
            encoding="utf-8"
        )

        self.assertLess(
            app.index("chatesp::ui::start("),
            app.index("device_memory_store.initialize()"),
        )


if __name__ == "__main__":
    unittest.main()
