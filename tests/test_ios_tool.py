from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tools.ios import (
    IOSToolError,
    available_ios_devices,
    configure_signing,
    local_team_id,
    project_team_ids,
    sanitized_output,
)


class IOSToolTests(unittest.TestCase):
    def setUp(self) -> None:
        self.team = "ABCD" + "EFGHIJ"

    def test_moves_one_team_to_the_ignored_configuration(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = root / "project.pbxproj"
            config = root / "Local.xcconfig"
            project.write_text(
                "before\n"
                f"\tDEVELOPMENT_TEAM = {self.team};\n"
                f"\tDEVELOPMENT_TEAM = {self.team};\n"
                "after\n",
                encoding="utf-8",
            )

            self.assertTrue(configure_signing(project, config))

            self.assertEqual(set(), project_team_ids(project.read_text()))
            self.assertEqual(self.team, local_team_id(config))
            self.assertEqual(0o600, config.stat().st_mode & 0o777)

    def test_rejects_multiple_project_teams(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = root / "project.pbxproj"
            config = root / "Local.xcconfig"
            other = "KLMN" + "OPQRST"
            project.write_text(
                f"DEVELOPMENT_TEAM = {self.team};\n"
                f"DEVELOPMENT_TEAM = {other};\n",
                encoding="utf-8",
            )
            with self.assertRaises(IOSToolError):
                configure_signing(project, config)

    def test_requires_one_local_team(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "Local.xcconfig"
            with self.assertRaises(IOSToolError):
                local_team_id(config)
            config.write_text("CODE_SIGN_STYLE = Automatic\n", encoding="utf-8")
            with self.assertRaises(IOSToolError):
                local_team_id(config)
            config.write_text(
                f"DEVELOPMENT_TEAM = {self.team}\n"
                f"DEVELOPMENT_TEAM = {self.team}\n",
                encoding="utf-8",
            )
            with self.assertRaises(IOSToolError):
                local_team_id(config)

    def test_existing_local_configuration_gets_private_permissions(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = root / "project.pbxproj"
            config = root / "Local.xcconfig"
            project.write_text("safe project\n", encoding="utf-8")
            config.write_text(
                f"DEVELOPMENT_TEAM = {self.team}\n", encoding="utf-8"
            )
            config.chmod(0o644)

            self.assertFalse(configure_signing(project, config))

            self.assertEqual(0o600, config.stat().st_mode & 0o777)

    def test_selects_only_available_physical_ios_devices(self) -> None:
        data = [
            {
                "platform": "com.apple.platform.iphoneos",
                "simulator": False,
                "available": True,
                "ignored": False,
                "identifier": "device-one",
                "name": "Phone One",
            },
            {
                "platform": "com.apple.platform.iphoneos",
                "simulator": False,
                "available": False,
                "ignored": False,
                "identifier": "device-two",
                "name": "Phone Two",
            },
            {
                "platform": "com.apple.platform.iphonesimulator",
                "simulator": True,
                "available": True,
                "ignored": False,
                "identifier": "simulator",
                "name": "Simulator",
            },
        ]
        devices = available_ios_devices(data)
        self.assertEqual(1, len(devices))
        self.assertEqual("device-one", devices[0].identifier)

    def test_sanitizes_local_values_and_home_paths(self) -> None:
        certificate = "Apple " + "Development: Example Person (LOCALTEAM)"
        device_id = "00008140-0011223344556677"
        home_path = "/" + "Users/example/file"
        text = (
            f"team={self.team} device=device-one {home_path}\n"
            f"person@example.com\n{certificate}\n{device_id}"
        )
        safe = sanitized_output(text, [self.team, "device-one"])
        self.assertNotIn(self.team, safe)
        self.assertNotIn("device-one", safe)
        self.assertNotIn(home_path, safe)
        self.assertNotIn("person@example.com", safe)
        self.assertNotIn("Example Person", safe)
        self.assertNotIn(device_id, safe)


if __name__ == "__main__":
    unittest.main()
