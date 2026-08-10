from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class IdleTimerContractTests(unittest.TestCase):
    def test_normal_turn_starts_idle_timer_after_all_response_actions(
        self,
    ) -> None:
        runtime = (ROOT / "firmware" / "main" / "voice_runtime.cpp").read_text(
            encoding="utf-8"
        )
        completion = runtime[
            runtime.rindex(
                "error = request_cancellation.normalize(wait_for_speech_worker())"
            ) : runtime.index("void log_turn_timing()")
        ]

        playback_finished = completion.index("wait_for_speech_worker()")
        speech_error_reported = completion.index("speech_error_message(error)")
        optional_image_joined = completion.index("join_image_worker();")
        idle_view_shown = completion.index("show_state(InteractionState::idle);")
        visual_published = completion.index("publish_selected_visual(")
        visual_error_reported = completion.rindex('"IMAGE UNAVAILABLE"')
        idle_timer_started = completion.index("interaction_.interaction_finished(")

        self.assertLess(playback_finished, speech_error_reported)
        self.assertLess(speech_error_reported, optional_image_joined)
        self.assertLess(optional_image_joined, idle_view_shown)
        self.assertLess(idle_view_shown, visual_published)
        self.assertLess(visual_published, visual_error_reported)
        self.assertLess(visual_error_reported, idle_timer_started)

    def test_error_starts_idle_timer_after_error_display(self) -> None:
        runtime = (ROOT / "firmware" / "main" / "voice_runtime.cpp").read_text(
            encoding="utf-8"
        )
        failure = runtime[
            runtime.index("void fail(const char *message)") :
            runtime.index("void refresh_settings()")
        ]

        self.assertLess(
            failure.index("show_error(message);"),
            failure.index("interaction_.fail(monotonic_ms());"),
        )

    def test_poweroff_error_starts_idle_timer_after_error_display(self) -> None:
        runtime = (ROOT / "firmware" / "main" / "voice_runtime.cpp").read_text(
            encoding="utf-8"
        )
        failure = runtime[
            runtime.index("void recover_poweroff(std::uint32_t now_ms)") :
            runtime.index("RuntimeSettings settings_")
        ]

        self.assertLess(
            failure.index('show_error("CHATESP COULD NOT TURN OFF");'),
            failure.index("interaction_.fail(monotonic_ms());"),
        )


if __name__ == "__main__":
    unittest.main()
