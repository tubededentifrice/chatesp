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
        self.assertIn("start_image_worker();", speech_case)
        self.assertIn(
            "image_tool_.take_selected_or_first(selected_image_result_)",
            prepare_visual,
        )

    def test_micropython_plot_reaches_the_display_after_speech(self) -> None:
        runtime = (ROOT / "firmware" / "main" / "voice_runtime.cpp").read_text(
            encoding="utf-8"
        )
        prepare_visual = runtime[
            runtime.index("void prepare_visual()") :
            runtime.index("void run_image_worker()")
        ]
        publish_visual = runtime[
            runtime.index("void publish_selected_visual(") :
            runtime.index("void finish_model_power_off()")
        ]
        interaction = runtime[
            runtime.index("error = request_cancellation.normalize(wait_for_speech_worker())") :
            runtime.index("void log_turn_timing()")
        ]

        self.assertLess(
            prepare_visual.index("python_tool_.take_plot(pending_plot_)"),
            prepare_visual.index(
                "image_tool_.take_selected_or_first(selected_image_result_)"
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
