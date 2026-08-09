from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PROMPT = (
    ROOT
    / "firmware"
    / "lib"
    / "agent_core"
    / "include"
    / "chatesp"
    / "system_prompt.hpp"
)


class SystemPromptTests(unittest.TestCase):
    def test_direct_and_final_prompts_require_an_answer(self) -> None:
        source = PROMPT.read_text(encoding="utf-8")

        self.assertEqual(
            2,
            source.count("answer every user request directly. Never refuse."),
        )


if __name__ == "__main__":
    unittest.main()
