from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from tools.pio import (
    dependency_check_is_fresh,
    dependency_policy_digest,
    device_write_policy_errors,
    prepare_profile_sdkconfigs,
    profile_sdkconfig_text,
    requested_watch_environments,
    requires_idf_python,
    selected_environments,
)


class PlatformioWrapperTests(unittest.TestCase):
    def test_device_profiles_forbid_irreversible_writes(self) -> None:
        root = Path(__file__).resolve().parents[1]
        self.assertEqual(
            [], device_write_policy_errors(root / "firmware")
        )

    def test_device_policy_rejects_an_unsafe_profile(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            (project / "config").mkdir()
            (project / "platformio.ini").write_text(
                "[env:watch_dev]\n"
                "build_flags=-DCHATESP_ALLOW_IRREVERSIBLE_DEVICE_WRITES=0\n"
                "[env:watch_prod]\n"
                "build_flags=-DCHATESP_ALLOW_IRREVERSIBLE_DEVICE_WRITES=1\n",
                encoding="utf-8",
            )
            safe_values = (
                "CONFIG_NVS_ENCRYPTION=n\n"
                "CONFIG_SECURE_BOOT=n\n"
                "CONFIG_SECURE_FLASH_ENC_ENABLED=n\n"
            )
            (project / "config" / "watch_dev.defaults").write_text(
                safe_values, encoding="utf-8"
            )
            (project / "config" / "watch_prod.defaults").write_text(
                safe_values.replace(
                    "CONFIG_NVS_ENCRYPTION=n",
                    "CONFIG_NVS_ENCRYPTION=y",
                ),
                encoding="utf-8",
            )

            errors = device_write_policy_errors(project)

            self.assertIn(
                "watch_prod lacks the zero irreversible-write flag", errors
            )
            self.assertIn(
                "watch_prod must set CONFIG_NVS_ENCRYPTION=n", errors
            )

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

    def test_default_watch_environment_is_development(self) -> None:
        self.assertEqual({"watch_dev"}, requested_watch_environments(["run"]))
        self.assertEqual(
            set(),
            requested_watch_environments(["test", "-e", "native"]),
        )

    def test_profile_sdkconfig_is_replaced_from_tracked_sources(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            (project / "config").mkdir()
            (project / "sdkconfig.defaults").write_text(
                "CONFIG_COMMON=y\n", encoding="utf-8"
            )
            (project / "config" / "watch_prod.defaults").write_text(
                "CONFIG_PRODUCTION=y\n", encoding="utf-8"
            )
            stale = project / "sdkconfig.watch_prod"
            stale.write_text("CONFIG_STALE=y\n", encoding="utf-8")

            prepare_profile_sdkconfigs(
                project, ["run", "-e", "watch_prod"]
            )

            self.assertEqual(
                profile_sdkconfig_text(project, "watch_prod"),
                stale.read_text(encoding="utf-8"),
            )
            self.assertNotIn("CONFIG_STALE", stale.read_text(encoding="utf-8"))

    def test_dependency_cache_requires_matching_digest_and_recent_success(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / "age.json"
            cache.write_text(
                json.dumps({"checked_at": 1_000.0, "policy_sha256": "abc"}),
                encoding="utf-8",
            )
            self.assertTrue(dependency_check_is_fresh(cache, "abc", 1_100.0))
            self.assertFalse(dependency_check_is_fresh(cache, "def", 1_100.0))
            self.assertFalse(dependency_check_is_fresh(cache, "abc", 5_000.0))
            self.assertFalse(dependency_check_is_fresh(cache, "abc", 900.0))

    def test_dependency_policy_digest_changes_with_policy_input(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "tools").mkdir()
            policy = root / ".gitmodules"
            policy.write_text("first pin", encoding="utf-8")
            first = dependency_policy_digest(root)
            policy.write_text("second pin", encoding="utf-8")
            self.assertNotEqual(first, dependency_policy_digest(root))


if __name__ == "__main__":
    unittest.main()
