from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class WakeLatencyContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.runtime = (
            ROOT / "firmware" / "main" / "voice_runtime.cpp"
        ).read_text(encoding="utf-8")

    def section(self, start: str, end: str) -> str:
        begin = self.runtime.index(start)
        return self.runtime[begin : self.runtime.index(end, begin)]

    def test_button_wake_posts_work_without_waiting(self) -> None:
        wake = self.section(
            "void wake_for_button(", "void request_display_wake("
        )

        self.assertIn("request_display_wake(now_ms)", wake)
        self.assertIn("ensure_ble_started()", wake)
        self.assertNotIn("wait_for_display", wake)
        self.assertNotIn("wait_for_ble", wake)
        self.assertNotIn("ui::wake", wake)
        self.assertNotIn("ble_provisioning::start", wake)

    def test_controller_workers_own_display_and_ble_transitions(self) -> None:
        display = self.section(
            "void perform_display_request(", "void run_display_controller()"
        )
        ble = self.section(
            "void perform_ble_request(", "void run_ble_controller()"
        )

        self.assertEqual(self.runtime.count("ui::wake("), 1)
        self.assertEqual(self.runtime.count("ui::sleep("), 1)
        self.assertIn("ui::wake(", display)
        self.assertIn("ui::sleep(", display)
        self.assertEqual(self.runtime.count("ble_provisioning::start("), 1)
        self.assertEqual(self.runtime.count("ble_provisioning::stop("), 1)
        self.assertIn("ble_provisioning::start(", ble)
        self.assertIn("ble_provisioning::stop(", ble)

    def test_recording_starts_before_display_recovery(self) -> None:
        recording = self.section(
            "void begin_recording(", "void capture_audio("
        )

        self.assertLess(
            recording.index("capture_.start()"),
            recording.index("request_display_wake(now_ms)"),
        )
        self.assertNotIn("capture_.discard()", recording)
        self.assertIn("CrashEvent::audio_capture_open", recording)

    def test_capture_prepare_keeps_the_codec_closed(self) -> None:
        source = (
            ROOT / "firmware" / "main" / "audio_capture.cpp"
        ).read_text(encoding="utf-8")
        prepare = source[
            source.index("esp_err_t AudioCapture::prepare()") :
            source.index("esp_err_t AudioCapture::start()")
        ]

        self.assertNotIn("esp_codec_dev_open", prepare)
        self.assertNotIn("esp_codec_dev_read", prepare)
        self.assertIn("heap_caps_calloc", prepare)
        self.assertNotIn("esp_codec_dev_set_in_gain", prepare)
        start = source[
            source.index("esp_err_t AudioCapture::start()") :
            source.index("esp_err_t AudioCapture::capture_chunk()")
        ]
        self.assertLess(
            start.index("try_acquire(AudioSession::capture)"),
            start.index("esp_codec_dev_set_in_gain"),
        )

    def test_short_release_discards_prepared_audio(self) -> None:
        command = self.section(
            "void process_command(", "void apply_device_context("
        )

        self.assertIn("prepare_recording();", command)
        self.assertIn("capture_.discard();", command)

    def test_new_press_supersedes_a_queued_old_release(self) -> None:
        command = self.section(
            "void process_command(", "void apply_device_context("
        )

        release = command[command.index("CommandKind::button_up") :]
        self.assertLess(
            release.index("button_pressed_.load"),
            release.index("interaction_.tick(command.at_ms)"),
        )
        self.assertIn("cancel_current();", release)

    def test_release_and_sleep_bound_startup_services(self) -> None:
        release = self.section(
            "void finish_recording_and_request()", "void finish_with_error("
        )
        sleep = self.section("void enter_sleep()", "bool stop_ble_for_request()")

        self.assertIn("finish_startup_services(true)", release)
        self.assertIn("kStartupServicesSleepWaitMs", sleep)
        self.assertIn("finish_startup_services(false)", sleep)
        self.assertLess(
            sleep.index("display_sleep_keep_panel_ready_ = true"),
            sleep.index("kStartupServicesSleepWaitMs"),
        )
        self.assertLess(
            release.index("finish_startup_services(true)"),
            release.index("settings_.has_wifi_credentials()"),
        )

    def test_recording_ui_never_waits_for_the_long_display_lock(self) -> None:
        display = self.section(
            "template <typename Callback>", "void prepare_deferred_ui()"
        )
        capture = self.section("void capture_audio(", "void finish_recording_and_request()")

        self.assertIn("? 0 : 100", display)
        self.assertIn("display_refresh_pending_ = true", display)
        self.assertNotIn("bsp_display_lock(100)", capture)

    def test_timing_events_are_content_free_and_named(self) -> None:
        names = (
            ROOT / "firmware" / "main" / "crash_diagnostics.cpp"
        ).read_text(encoding="utf-8")
        for event in (
            "startup_pwr_credit",
            "startup_panel_ready",
            "startup_first_pixel",
            "startup_capture_ready",
            "startup_services_ready",
            "audio_prepare_begin",
            "audio_prepare_complete",
            "audio_capture_open",
            "audio_capture_read_begin",
            "audio_first_chunk",
        ):
            self.assertIn(f'return "{event}"', names)

    def test_release_uses_one_phone_proxy_deadline(self) -> None:
        release = self.section(
            "void finish_recording_and_request()", "void finish_with_error("
        )

        released = release.index("released_at_ms = monotonic_ms()")
        ble_wait = release.index("wait_for_ble_until(")
        proxy_wait = release.index("monotonic_ms() - released_at_ms")
        self.assertLess(released, ble_wait)
        self.assertLess(ble_wait, proxy_wait)
        self.assertNotIn("proxy_wait_started_ms", release)

    def test_deferred_ui_does_not_overlap_capture_or_ble_start(self) -> None:
        deferred = self.section(
            "void prepare_deferred_ui()", "void prepare_deferred_ui_for_request()"
        )
        ble = self.section("bool ensure_ble_started()", "bool reserve_ble_restart_memory()")

        self.assertIn("interaction_.state() != InteractionState::idle", deferred)
        self.assertIn("voice_priority_.load", deferred)
        self.assertIn("!ble_request_complete(ble_generation)", deferred)
        self.assertIn("deferred_ui_task_ != nullptr", ble)
        self.assertIn("deferred_ui_cancelled", self.runtime)
        self.assertIn(
            "ui::prepare_deferred_views(deferred_ui_cancelled, this)",
            self.runtime,
        )

    def test_hard_display_off_retries_the_hard_request(self) -> None:
        sleep = self.section("void enter_sleep()", "bool stop_ble_for_request()")
        retry = self.section(
            "void retry_display_sleep(", "void process_state_change("
        )

        self.assertIn("display_sleep_keep_panel_ready_ = true", sleep)
        self.assertIn("display_sleep_keep_panel_ready_ = false", sleep)
        self.assertIn(
            "display_sleep_generation_ = request_display(", sleep
        )
        self.assertIn("display_sleep_keep_panel_ready_", retry)
        self.assertNotIn("request_display(false, true", retry)

    def test_controller_failure_never_runs_display_commands_on_callers(self) -> None:
        display = self.section(
            "std::uint32_t request_display(", "void perform_display_request("
        )
        ble = self.section(
            "std::uint32_t request_ble(", "void perform_ble_request("
        )

        self.assertNotIn("perform_display_request", display)
        self.assertIn("ESP_ERR_INVALID_STATE", display)
        self.assertIn("button_pressed_.load", ble)
        self.assertIn("InteractionState::recording", ble)
        self.assertIn("display_completed_generation_.store", display)
        self.assertIn("ble_completed_generation_.store", ble)

    def test_failed_touch_enables_controls_only_once(self) -> None:
        deferred = self.section(
            "void prepare_deferred_ui()", "void prepare_deferred_ui_for_request()"
        )

        self.assertIn("!quick_controls_attempted_", deferred)
        self.assertLess(
            deferred.index("quick_controls_attempted_ = true"),
            deferred.index("ui::enable_quick_controls"),
        )
        self.assertIn("if (quick_controls_enabled_)", deferred)


if __name__ == "__main__":
    unittest.main()
