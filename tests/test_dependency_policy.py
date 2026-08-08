from __future__ import annotations

import subprocess
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path
from unittest.mock import patch

from tools.check_dependency_age import (
    PolicyError,
    check_component_registry_reference,
    check_github_actions,
    check_github_release,
    check_git_submodules,
    check_hashed_requirements,
    check_idf_lock,
    check_idf_manifests,
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

    def test_accepts_pinned_initialized_git_submodule(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            checkout = root / "firmware" / "third_party" / "runtime"
            checkout.mkdir(parents=True)
            commit = "0123456789abcdef0123456789abcdef01234567"
            (root / ".gitmodules").write_text(
                '[submodule "runtime"]\n'
                "\tpath = firmware/third_party/runtime\n"
                "\turl = https://github.com/vendor/runtime.git\n"
                f"\tcommit = {commit}\n"
            )
            completed = subprocess.CompletedProcess(
                args=[], returncode=0, stdout=f"{commit}\n", stderr=""
            )
            with patch("tools.check_dependency_age.subprocess.run", return_value=completed):
                self.assertEqual(
                    {("vendor", "runtime", commit)}, check_git_submodules(root)
                )

    def test_rejects_mutable_or_mismatched_git_submodule(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            checkout = root / "runtime"
            checkout.mkdir()
            (root / ".gitmodules").write_text(
                '[submodule "runtime"]\n'
                "\tpath = runtime\n"
                "\turl = https://github.com/vendor/runtime.git\n"
                "\tcommit = main\n"
            )
            with self.assertRaises(PolicyError):
                check_git_submodules(root)

            commit = "0123456789abcdef0123456789abcdef01234567"
            (root / ".gitmodules").write_text(
                '[submodule "runtime"]\n'
                "\tpath = runtime\n"
                "\turl = https://github.com/vendor/runtime.git\n"
                f"\tcommit = {commit}\n"
            )
            completed = subprocess.CompletedProcess(
                args=[], returncode=0, stdout=f"{'f' * 40}\n", stderr=""
            )
            with patch("tools.check_dependency_age.subprocess.run", return_value=completed):
                with self.assertRaises(PolicyError):
                    check_git_submodules(root)

    def test_requires_hash_lock_for_firmware_project(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            firmware = root / "firmware"
            firmware.mkdir()
            (firmware / "platformio.ini").write_text("[env:watch]\n")
            with self.assertRaises(PolicyError):
                check_hashed_requirements(root, datetime.now(timezone.utc))

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

    def test_accepts_full_idf_component_git_pin(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            main = root / "firmware" / "main"
            main.mkdir(parents=True)
            (main / "idf_component.yml").write_text(
                "dependencies:\n"
                "  vendor/board:\n"
                "    git: https://github.com/vendor/components.git\n"
                "    path: bsp/board\n"
                "    version: 0123456789abcdef0123456789abcdef01234567\n"
            )
            references, expected = check_idf_manifests(root)
            self.assertEqual(
                {("vendor", "components", "0123456789abcdef0123456789abcdef01234567")},
                references,
            )
            self.assertEqual(
                ("git", "0123456789abcdef0123456789abcdef01234567"),
                expected["vendor/board"],
            )

    def test_rejects_mutable_idf_component_pin(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            main = root / "firmware" / "main"
            main.mkdir(parents=True)
            (main / "idf_component.yml").write_text(
                "dependencies:\n  vendor/board:\n    version: ^2.0.0\n"
            )
            with self.assertRaises(PolicyError):
                check_idf_manifests(root)

    def test_ignores_downloaded_idf_component_manifests(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            managed = root / "firmware" / "managed_components" / "vendor__driver"
            managed.mkdir(parents=True)
            (managed / "idf_component.yml").write_text(
                "dependencies:\n  vendor/helper:\n    version: ^1\n"
            )
            references, expected = check_idf_manifests(root)
            self.assertEqual(set(), references)
            self.assertEqual({}, expected)

    def test_ignores_idf_manifests_inside_a_pinned_submodule(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            submodule = root / "firmware" / "third_party" / "runtime"
            submodule.mkdir(parents=True)
            (root / ".gitmodules").write_text(
                '[submodule "runtime"]\n'
                "\tpath = firmware/third_party/runtime\n"
                "\turl = https://github.com/vendor/runtime.git\n"
                f"\tcommit = {'0' * 40}\n"
            )
            (submodule / "idf_component.yml").write_text(
                "dependencies:\n  vendor/helper:\n    version: ^1\n"
            )
            references, expected = check_idf_manifests(root)
            self.assertEqual(set(), references)
            self.assertEqual({}, expected)

    def test_requires_idf_registry_component_hash(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            firmware = root / "firmware"
            firmware.mkdir()
            (firmware / "dependencies.lock").write_text(
                "dependencies:\n"
                "  vendor/driver:\n"
                "    dependencies:\n"
                "    - name: idf\n"
                "      version: '>=5.2'\n"
                "    source:\n"
                "      registry_url: https://components.espressif.com\n"
                "      type: service\n"
                "    version: 1.2.3\n"
                "direct_dependencies:\n"
            )
            with self.assertRaises(PolicyError):
                check_idf_lock(root, {"vendor/driver": ("registry", "1.2.3")})

    def test_checks_idf_component_hash_and_cooldown(self) -> None:
        metadata = {
            "namespace": "vendor",
            "name": "driver",
            "versions": [
                {
                    "version": "1.2.3",
                    "component_hash": "a" * 64,
                    "created_at": "2026-06-01T00:00:00Z",
                }
            ],
        }
        with patch("tools.check_dependency_age.fetch_json", return_value=metadata):
            check_component_registry_reference(
                ("vendor", "driver", "1.2.3", "a" * 64),
                datetime(2026, 7, 1, tzinfo=timezone.utc),
            )
        with patch("tools.check_dependency_age.fetch_json", return_value=metadata):
            with self.assertRaises(PolicyError):
                check_component_registry_reference(
                    ("vendor", "driver", "1.2.3", "b" * 64),
                    datetime(2026, 7, 1, tzinfo=timezone.utc),
                )


if __name__ == "__main__":
    unittest.main()
