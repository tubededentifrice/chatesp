from __future__ import annotations

import unittest

from tools.pio import requires_idf_python, selected_environments


class PlatformioWrapperTests(unittest.TestCase):
    def test_reads_short_and_long_environment_options(self) -> None:
        self.assertEqual(
            {"watch_dev", "native", "watch_prod"},
            selected_environments(
                [
                    "run",
                    "-e",
                    "watch_dev",
                    "--environment=native",
                    "--environment",
                    "watch_prod",
                ]
            ),
        )

    def test_default_environment_prepares_idf_python(self) -> None:
        self.assertTrue(requires_idf_python(["run"]))

    def test_watch_environment_prepares_idf_python(self) -> None:
        self.assertTrue(requires_idf_python(["run", "-e", "watch_dev"]))
        self.assertTrue(requires_idf_python(["run", "-e", "watch_prod"]))

    def test_native_environment_skips_idf_python(self) -> None:
        self.assertFalse(requires_idf_python(["test", "-e", "native"]))


if __name__ == "__main__":
    unittest.main()
