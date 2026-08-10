from __future__ import annotations

import subprocess
import unittest
from pathlib import Path
from unittest.mock import patch

from tools.project_version import current_project_version


class ProjectVersionTests(unittest.TestCase):
    def test_uses_the_exact_esp_idf_git_description(self) -> None:
        completed = subprocess.CompletedProcess(
            args=[], returncode=0, stdout="v1.2.3-4-gabc1234-dirty\n"
        )

        with patch(
            "tools.project_version.subprocess.run", return_value=completed
        ) as run:
            version = current_project_version(Path("/repo"))

        self.assertEqual("v1.2.3-4-gabc1234-dirty", version)
        run.assert_called_once_with(
            ["git", "describe", "--always", "--tags", "--dirty"],
            cwd=Path("/repo"),
            check=True,
            capture_output=True,
            text=True,
        )


if __name__ == "__main__":
    unittest.main()
