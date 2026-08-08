from __future__ import annotations

import plistlib
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PROJECT_FILE = ROOT / "ios" / "ChatESP.xcodeproj" / "project.pbxproj"
INFO_PLIST = ROOT / "ios" / "ChatESP" / "Info.plist"
APP_CONFIGURATION_IDS = (
    "D00000000000000000000050",  # Debug
    "D00000000000000000000051",  # Release
)


class IOSProjectMetadataTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.project = PROJECT_FILE.read_text(encoding="utf-8")
        with INFO_PLIST.open("rb") as stream:
            cls.info = plistlib.load(stream)

    def app_build_settings(self, configuration_id: str) -> str:
        match = re.search(
            rf"{configuration_id} /\* (?:Debug|Release) \*/ = \{{"
            r"\s*isa = XCBuildConfiguration;\s*buildSettings = \{"
            r"(?P<settings>.*?)\n\s*\};",
            self.project,
            re.DOTALL,
        )
        self.assertIsNotNone(match, f"Missing app configuration {configuration_id}")
        return match.group("settings")

    def test_app_generates_a_launch_screen_in_all_configurations(self) -> None:
        for configuration_id in APP_CONFIGURATION_IDS:
            with self.subTest(configuration_id=configuration_id):
                settings = self.app_build_settings(configuration_id)
                self.assertIn(
                    "INFOPLIST_KEY_UILaunchScreen_Generation = YES;", settings
                )

    def test_app_declares_bluetooth_central_background_mode(self) -> None:
        self.assertEqual(["bluetooth-central"], self.info.get("UIBackgroundModes"))
        self.assertNotIn("Info.plist in Resources", self.project)
        for configuration_id in APP_CONFIGURATION_IDS:
            with self.subTest(configuration_id=configuration_id):
                settings = self.app_build_settings(configuration_id)
                self.assertIn("GENERATE_INFOPLIST_FILE = YES;", settings)
                self.assertIn("INFOPLIST_FILE = ChatESP/Info.plist;", settings)

    def test_project_does_not_store_an_apple_development_team(self) -> None:
        self.assertNotRegex(self.project, r"\bDEVELOPMENT_TEAM\s*=")


if __name__ == "__main__":
    unittest.main()
