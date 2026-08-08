from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class NetworkContextLifecycleTests(unittest.TestCase):
    def test_worker_creation_failure_does_not_retry_the_lookup(self) -> None:
        runtime = (ROOT / "firmware" / "main" / "voice_runtime.cpp").read_text(
            encoding="utf-8"
        )
        creation_failure = runtime[
            runtime.index("if (created != pdPASS) {", runtime.index("network_context_task_entry")) :
            runtime.index("void run_network_context_worker()")
        ]

        self.assertNotIn("context_lookup_attempted_ = false", creation_failure)
        self.assertIn("network_context_finished_at_ms_ = monotonic_ms()", creation_failure)
        self.assertIn("ensure_ble_started();", creation_failure)


if __name__ == "__main__":
    unittest.main()
