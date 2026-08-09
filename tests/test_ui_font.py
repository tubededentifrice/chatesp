from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class UiFontTests(unittest.TestCase):
    def test_answer_font_contains_common_model_punctuation(self) -> None:
        font = (ROOT / "firmware" / "main" / "chatesp_font_18.c").read_text(
            encoding="utf-8"
        )

        self.assertIn("0x2000-0x206F", font)
        for code_point in (
            "2013",  # en dash
            "2014",  # em dash
            "2018",  # left single quotation mark
            "2019",  # right single quotation mark
            "201C",  # left double quotation mark
            "201D",  # right double quotation mark
            "2022",  # bullet
            "2026",  # ellipsis
        ):
            self.assertIn(f"U+{code_point}", font)


if __name__ == "__main__":
    unittest.main()
