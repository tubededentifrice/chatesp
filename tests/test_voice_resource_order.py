from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class VoiceResourceOrderTests(unittest.TestCase):
    def test_saved_settings_apply_before_startup_input_is_processed(self) -> None:
        runtime = (ROOT / "firmware" / "main" / "voice_runtime.cpp").read_text(
            encoding="utf-8"
        )
        start = runtime[
            runtime.index(
                "esp_err_t start(bool startup_button_down, std::uint32_t startup_at_ms)"
            ) : runtime.index("void action_button_edge(")
        ]

        run = runtime[
            runtime.index("void run()") : runtime.index("void process_command(")
        ]

        self.assertLess(
            start.index("settings_store_.initialize()"),
            start.index("xTaskCreatePinnedToCore(\n            task_entry"),
        )
        self.assertNotIn("refresh_settings();", start)
        self.assertLess(
            run.index("refresh_settings();"),
            run.index("interaction_.ready("),
        )
        self.assertLess(
            run.index("refresh_settings();"),
            run.index("if (button_pressed_.load("),
        )

    def test_speech_start_event_follows_successful_audio_start(self) -> None:
        runtime = (ROOT / "firmware" / "main" / "voice_runtime.cpp").read_text(
            encoding="utf-8"
        )
        sink = runtime[
            runtime.index("class SpeechStartSink final") :
            runtime.index("class AtomicCancellation final")
        ]

        audio_start = sink.index("sink_.begin(")
        success_check = sink.index("result == agent::Error::none")
        speech_event = sink.index("agent_loop_.report_speech_start();")
        self.assertLess(audio_start, success_check)
        self.assertLess(success_check, speech_event)

    def test_optional_image_work_starts_only_after_speech_starts(self) -> None:
        runtime = (ROOT / "firmware" / "main" / "voice_runtime.cpp").read_text(
            encoding="utf-8"
        )
        prepare_visual = runtime[
            runtime.index("void prepare_visual()") :
            runtime.index("void run_image_worker()")
        ]
        answer_case = runtime[
            runtime.index("case agent::AgentProgressEvent::answer_start:") :
            runtime.index("case agent::AgentProgressEvent::answer_ready:")
        ]
        speech_case = runtime[
            runtime.index("case agent::AgentProgressEvent::speech_start:") :
            runtime.index("agent::Error write_chat_text")
        ]

        self.assertNotIn("start_image_worker();", answer_case)
        self.assertIn("start_image_worker(*speech_cancellation_);", speech_case)
        self.assertIn(
            "image_tool_.take_selected_or_first_candidates(image_candidates_)",
            prepare_visual,
        )

    def test_image_candidate_recovery_is_bounded(self) -> None:
        runtime = (ROOT / "firmware" / "main" / "voice_runtime.cpp").read_text(
            encoding="utf-8"
        )
        worker = runtime[
            runtime.index("void run_image_worker()") :
            runtime.index("void join_image_worker()")
        ]

        self.assertIn("kMaximumImageCandidateAttempts = 3", runtime)
        self.assertIn("kImageCandidateBudgetMs = 20'000", runtime)
        self.assertIn(
            "image_candidates_.size, kMaximumImageCandidateAttempts", worker
        )
        self.assertIn("monotonic_ms() - started_ms", worker)
        self.assertIn("kImageCandidateBudgetMs - elapsed_ms", worker)
        self.assertIn("policy.total_timeout_ms = remaining_budget_ms;", worker)
        self.assertIn("policy.max_attempts = 1;", worker)
        self.assertIn("image_deadline.cancelled()", worker)
        self.assertIn("JpegImageSink sink(image_deadline)", worker)
        self.assertNotIn("ESP_LOG", worker)

    def test_image_recovery_runs_after_an_early_speech_failure(self) -> None:
        runtime = (ROOT / "firmware" / "main" / "voice_runtime.cpp").read_text(
            encoding="utf-8"
        )
        interaction = runtime[
            runtime.index(
                "error = request_cancellation.normalize(wait_for_speech_worker())"
            ) : runtime.index("bool speech_failed = false;")
        ]

        speech_wait = interaction.index("wait_for_speech_worker()")
        image_start = interaction.index("start_image_worker(request_cancellation);")
        image_join = interaction.index("join_image_worker();")
        self.assertLess(speech_wait, image_start)
        self.assertLess(image_start, image_join)
        self.assertIn("!speech_started_for_turn_.load", interaction)
        self.assertIn("!request_cancellation.cancelled()", interaction)

    def test_image_failure_notice_keeps_speech_failure_priority(self) -> None:
        runtime = (ROOT / "firmware" / "main" / "voice_runtime.cpp").read_text(
            encoding="utf-8"
        )
        completion = runtime[
            runtime.index("if (!speech_failed) {") :
            runtime.index("timing_.mark(runtime::TurnPhase::completion")
        ]

        self.assertIn('"IMAGE UNAVAILABLE"', completion)
        self.assertIn("ui::show_answer_notice(", completion)
        self.assertIn("const bool visual_published", completion)
        self.assertIn("if (!visual_published && !speech_failed)", completion)

    def test_cleanup_clears_all_runtime_image_state(self) -> None:
        runtime = (ROOT / "firmware" / "main" / "voice_runtime.cpp").read_text(
            encoding="utf-8"
        )
        cleanup = runtime[
            runtime.index("void clear_runtime_image_state()") :
            runtime.index("void prepare_visual()")
        ]

        self.assertIn("image_candidates_.clear();", cleanup)
        self.assertIn("image_frame_.reset();", cleanup)
        self.assertIn("image_requested_.store(false", cleanup)
        self.assertIn("image_unavailable_.store(false", cleanup)
        self.assertIn("image_tool_.clear_results();", cleanup)
        self.assertLess(
            cleanup.index("image_transport_.cancel_active();"),
            cleanup.index("join_image_worker();"),
        )
        self.assertLess(
            cleanup.index("join_image_worker();"),
            cleanup.index("clear_runtime_image_state();"),
        )
        for function_name in (
            "void cancel_current()",
            "void fail(const char *message)",
            "void enter_sleep()",
        ):
            function = runtime[
                runtime.index(function_name) :
                runtime.index("\n    }", runtime.index(function_name))
            ]
            self.assertIn("clear_all_image_state();", function)

    def test_micropython_plot_reaches_the_display_after_speech(self) -> None:
        runtime = (ROOT / "firmware" / "main" / "voice_runtime.cpp").read_text(
            encoding="utf-8"
        )
        prepare_visual = runtime[
            runtime.index("void prepare_visual()") :
            runtime.index("void run_image_worker()")
        ]
        publish_visual = runtime[
            runtime.index("bool publish_selected_visual(") :
            runtime.index("void finish_model_power_off()")
        ]
        interaction = runtime[
            runtime.index("error = request_cancellation.normalize(wait_for_speech_worker())") :
            runtime.index("void log_turn_timing()")
        ]

        self.assertLess(
            prepare_visual.index("python_tool_.take_plot(pending_plot_)"),
            prepare_visual.index(
                "image_tool_.take_selected_or_first_candidates(image_candidates_)"
            ),
        )
        self.assertIn("ui::show_fullscreen_plot(plot)", publish_visual)
        self.assertLess(
            interaction.index("wait_for_speech_worker()"),
            interaction.index("publish_selected_visual(image_frame_, pending_plot_);"),
        )

    def test_speech_ui_does_not_show_numeric_error_codes(self) -> None:
        runtime = (ROOT / "firmware" / "main" / "voice_runtime.cpp").read_text(
            encoding="utf-8"
        )

        self.assertNotIn('"SPEECH ERROR %u"', runtime)
        self.assertIn("speech_error_message(error)", runtime)


if __name__ == "__main__":
    unittest.main()
