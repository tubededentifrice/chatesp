from __future__ import annotations

import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path
from unittest.mock import patch

from tools.check_dependency_age import (
    PolicyError,
    check_github_actions,
    check_github_release,
    check_platformio,
    check_pyproject,
    check_uv_lock,
)


VALID_PROJECT = """
[project]
name = "test-tools"
version = "0.1.0"
requires-python = ">=3.13,<3.14"
dependencies = ["platformio==6.1.19"]

[tool.uv]
package = false
exclude-newer = "2 weeks"
required-version = ">=0.11.32"
"""


class DependencyPolicyTests(unittest.TestCase):
    def test_accepts_exact_python_pin_and_uv_policy(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "pyproject.toml").write_text(VALID_PROJECT)
            references, packages = check_pyproject(root)
            self.assertEqual(set(), references)
            self.assertEqual(("registry", "6.1.19"), packages["platformio"])

    def test_rejects_loose_uv_version(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = VALID_PROJECT.replace('required-version = ">=0.11.32"', 'required-version = ">=0.12.0"')
            (root / "pyproject.toml").write_text(project)
            with self.assertRaises(PolicyError):
                check_pyproject(root)

    def test_rejects_mutable_python_pin(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = VALID_PROJECT.replace("platformio==6.1.19", "platformio>=6.1")
            (root / "pyproject.toml").write_text(project)
            with self.assertRaises(PolicyError):
                check_pyproject(root)

    def test_requires_two_week_lock_span(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "uv.lock").write_text(
                'version = 1\n[options]\nexclude-newer-span = "P1W"\n'
            )
            with self.assertRaises(PolicyError):
                check_uv_lock(root, datetime.now(timezone.utc), {})

    def test_requires_full_github_action_commit(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            workflows = root / ".github" / "workflows"
            workflows.mkdir(parents=True)
            workflow = workflows / "test.yml"
            workflow.write_text("steps:\n  - uses: actions/checkout@v4\n")
            with self.assertRaises(PolicyError):
                check_github_actions(root)

    def test_requires_exact_ci_uv_version(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            workflows = root / ".github" / "workflows"
            workflows.mkdir(parents=True)
            workflow = workflows / "test.yml"
            workflow.write_text(
                "steps:\n"
                "  - uses: astral-sh/setup-uv@"
                "d0cc045d04ccac9d8b7881df0226f9e82c39688e\n"
                "    with:\n"
                '      version: "0.11.31"\n'
            )
            with self.assertRaises(PolicyError):
                check_github_actions(root)

    def test_requires_ci_uv_release_to_finish_cooldown(self) -> None:
        cutoff = datetime(2026, 7, 24, tzinfo=timezone.utc)
        metadata = {
            "tag_name": "0.11.32",
            "published_at": "2026-07-23T23:17:45Z",
        }
        with patch("tools.check_dependency_age.fetch_json", return_value=metadata):
            check_github_release("astral-sh", "uv", "0.11.32", cutoff)
        with patch("tools.check_dependency_age.fetch_json", return_value=metadata):
            with self.assertRaises(PolicyError):
                check_github_release(
                    "astral-sh",
                    "uv",
                    "0.11.32",
                    datetime(2026, 7, 23, tzinfo=timezone.utc),
                )

    def test_accepts_exact_platformio_pin(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "platformio.ini").write_text(
                "[env:test]\nplatform = platformio/espressif32@6.10.0\n"
            )
            references, git_references = check_platformio(root)
            self.assertEqual(
                {("platform", "platformio", "espressif32", "6.10.0")},
                references,
            )
            self.assertEqual(set(), git_references)

    def test_rejects_platformio_dependency_substitution(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "platformio.ini").write_text(
                "[env:test]\nplatform = ${sysenv.PLATFORM_SPEC}\n"
            )
            with self.assertRaises(PolicyError):
                check_platformio(root)


if __name__ == "__main__":
    unittest.main()
