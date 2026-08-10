from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class FirmwareMemoryBudgetTests(unittest.TestCase):
    def test_display_buffer_keeps_internal_audio_and_ble_budget(self) -> None:
        settings = (ROOT / "firmware" / "sdkconfig.defaults").read_text(
            encoding="utf-8"
        )
        board = (
            ROOT
            / "firmware"
            / "components"
            / "chatesp_board"
            / "esp32_s3_touch_amoled_1_8.c"
        ).read_text(encoding="utf-8")

        self.assertIn("CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT=32", settings)
        self.assertIn(".buff_dma = true,", board)
        self.assertIn(".buff_spiram = false,", board)

    def test_audio_dma_is_reserved_before_runtime_tasks(self) -> None:
        runtime = (ROOT / "firmware" / "main" / "voice_runtime.cpp").read_text(
            encoding="utf-8"
        )

        self.assertLess(
            runtime.index("capture_.initialize()"),
            runtime.index("command_queue_ = xQueueCreate"),
        )

    def test_startup_storage_stack_is_released_before_runtime_tasks(self) -> None:
        runtime = (ROOT / "firmware" / "main" / "voice_runtime.cpp").read_text(
            encoding="utf-8"
        )
        startup = runtime.index("start_startup_services();")
        release = runtime.index("release_startup_services_stack()", startup)
        passkey = runtime.index("const BaseType_t passkey_task_result", release)
        voice = runtime.index("const BaseType_t task_result", passkey)

        self.assertLess(startup, release)
        self.assertLess(release, passkey)
        self.assertLess(passkey, voice)
        self.assertIn("kStartupServicesStackBytes = 12 * 1024", runtime)
        self.assertIn("kRuntimeStackBytes = 28 * 1024", runtime)
        self.assertIn("pdMS_TO_TICKS(kControllerWaitMs)", runtime)

    def test_passkey_ui_has_stack_for_a_synchronous_display_refresh(self) -> None:
        runtime = (ROOT / "firmware" / "main" / "voice_runtime.cpp").read_text(
            encoding="utf-8"
        )

        self.assertIn("kPasskeyStackBytes = 8 * 1024", runtime)
        passkey_task = runtime[
            runtime.index("const BaseType_t passkey_task_result") : runtime.index(
                "if (passkey_task_result != pdPASS)"
            )
        ]
        self.assertIn("kPasskeyStackBytes", passkey_task)

    def test_ble_memory_is_released_before_transcription(self) -> None:
        runtime = (ROOT / "firmware" / "main" / "voice_runtime.cpp").read_text(
            encoding="utf-8"
        )

        self.assertLess(
            runtime.index("stop_ble_for_request()"),
            runtime.index("transcription_provider_.transcribe("),
        )

    def test_optional_timezone_worker_uses_its_bounded_psram_stack(self) -> None:
        runtime = (ROOT / "firmware" / "main" / "voice_runtime.cpp").read_text(
            encoding="utf-8"
        )
        lookup = runtime[
            runtime.index("void start_network_context_lookup()") :
            runtime.index("void run_network_context_worker()")
        ]

        self.assertIn("network_context_task_entry", lookup)
        self.assertIn("kNetworkContextStackBytes", lookup)
        self.assertIn("MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT", lookup)
        self.assertIn("vTaskDeleteWithCaps(completed_task)", runtime)

    def test_board_audio_allocation_has_no_fatal_check(self) -> None:
        board = (
            ROOT
            / "firmware"
            / "components"
            / "chatesp_board"
            / "esp32_s3_touch_amoled_1_8.c"
        ).read_text(encoding="utf-8")
        audio_init = board[
            board.index("esp_err_t bsp_audio_init") :
            board.index("esp_codec_dev_handle_t bsp_audio_codec_speaker_init")
        ]

        self.assertNotIn("ESP_ERROR_CHECK", audio_init)
        self.assertIn("goto fail;", audio_init)


if __name__ == "__main__":
    unittest.main()
