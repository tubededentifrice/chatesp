from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "firmware" / "main" / "voice_runtime.cpp"


class BlePasskeyLifecycleTests(unittest.TestCase):
    def test_passkey_task_leaves_refresh_work_to_the_lvgl_task(self) -> None:
        source = RUNTIME.read_text(encoding="utf-8")
        start = source.index("void run_passkey_ui()")
        end = source.index(
            "void sync_quick_controls_to_confirmed_state", start
        )
        task = source[start:end]

        self.assertNotIn("with_display", task)
        self.assertNotIn("lv_refr_now", task)
        self.assertIn("bsp_display_lock(100)", task)
        self.assertIn("bsp_display_unlock()", task)

    def test_pairing_activity_wakes_an_unavailable_display(self) -> None:
        source = RUNTIME.read_text(encoding="utf-8")
        start = source.index(
            "if (command.kind == CommandKind::provisioning_activity)"
        )
        end = source.index(
            "if (command.kind == CommandKind::toggle_mode)", start
        )
        handler = source[start:end]

        self.assertIn("display_available_.store(true", handler)
        self.assertIn("poweroff_gate_.recover()", handler)
        self.assertIn("request_display_wake(command.at_ms)", handler)
        self.assertIn("interaction_.note_idle_activity(command.at_ms)", handler)


if __name__ == "__main__":
    unittest.main()
