import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware" / "main"


class FirmwareTaskBudgetTests(unittest.TestCase):
    def test_all_product_tasks_have_a_reviewed_descriptor(self) -> None:
        header = (MAIN / "task_config.hpp").read_text()
        expected = {
            "voice_runtime",
            "lvgl",
            "ble_passkey_ui",
            "display_control",
            "ble_control",
            "startup_services",
            "image_download",
            "wifi_recording",
            "network_context",
            "tts_requests",
            "deferred_ui",
            "tts_playback",
            "ble_stop",
        }
        declared = set(re.findall(r"inline constexpr TaskSpec (\w+)\{", header))
        self.assertEqual(expected, declared)
        self.assertIn("lvgl_port_config()", header)
        self.assertNotIn("ESP_LVGL_PORT_INIT_CONFIG", header)

    def test_task_creation_uses_the_reviewed_contract(self) -> None:
        sources = "\n".join(
            path.read_text() for path in sorted(MAIN.glob("*.cpp"))
        )
        self.assertNotIn("xTaskCreate", sources)
        calls = re.findall(r"task_config::create\((.*?)\);", sources, re.DOTALL)
        self.assertGreaterEqual(len(calls), 12)
        for call in calls:
            descriptors = re.findall(r"task_config::([a-z_]+)", call)
            self.assertEqual(1, len(descriptors), call)

        header = (MAIN / "task_config.hpp").read_text()
        wrapper = header[header.index("inline BaseType_t create(") :]
        for field in (
            "spec.name",
            "spec.stack_bytes",
            "spec.priority",
            "spec.core",
            "spec.stack_caps",
        ):
            self.assertIn(field, wrapper)

    def test_resource_telemetry_has_fixed_private_content_free_coverage(
        self,
    ) -> None:
        telemetry = (MAIN / "resource_telemetry.cpp").read_text()
        runtime = (MAIN / "voice_runtime.cpp").read_text()

        for field in (
            "internal_min",
            "largest_min",
            "psram_min",
            "submission",
            "completion",
            "transfer_error",
            "queue_coalesced",
            "touch_error",
            "lock_timeout",
            "pmic_i2c_error",
        ):
            self.assertIn(field, telemetry)
        for forbidden in ("nvs_", "transcript", "answer", "coordinate"):
            self.assertNotIn(forbidden, telemetry.lower())
        self.assertIn('log_summary("turn")', runtime)
        self.assertIn('log_summary("soak")', runtime)
        for task_id in (
            "voice_runtime",
            "ble_passkey_ui",
            "display_control",
            "ble_control",
            "startup_services",
            "image_download",
            "wifi_recording",
            "network_context",
            "tts_requests",
            "deferred_ui",
        ):
            self.assertIn(f"TaskId::{task_id}", runtime)
        ui = (MAIN / "ui.cpp").read_text()
        self.assertIn("TaskId::lvgl", ui)


if __name__ == "__main__":
    unittest.main()
