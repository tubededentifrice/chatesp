from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class IdleTimerContractTests(unittest.TestCase):
    def test_normal_turn_starts_idle_timer_after_playback_before_image_join(
        self,
    ) -> None:
        runtime = (ROOT / "firmware" / "main" / "voice_runtime.cpp").read_text(
            encoding="utf-8"
        )
        completion = runtime[
            runtime.index(
                "error = request_cancellation.normalize(wait_for_speech_worker())"
            ) : runtime.index("void log_turn_timing()")
        ]

        playback_finished = completion.index("wait_for_speech_worker()")
        speech_error_reported = completion.index("speech_error_message(error)")
        idle_timer_started = completion.index("interaction_.interaction_finished(")
        optional_image_joined = completion.index("join_image_worker();")

        self.assertLess(playback_finished, speech_error_reported)
        self.assertLess(speech_error_reported, idle_timer_started)
        self.assertLess(idle_timer_started, optional_image_joined)


if __name__ == "__main__":
    unittest.main()
