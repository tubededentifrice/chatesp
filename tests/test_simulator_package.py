from __future__ import annotations

from pathlib import Path
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
SIMULATOR_ROOT = REPOSITORY_ROOT / "simulator"


class SimulatorPackageTests(unittest.TestCase):
    def test_package_has_a_standalone_boundary(self) -> None:
        cmake = (SIMULATOR_ROOT / "CMakeLists.txt").read_text()
        self.assertIn("CHATESP_APP_CORE_DIR", cmake)
        self.assertNotIn("firmware/main", cmake)
        self.assertTrue((SIMULATOR_ROOT / "LICENSE").is_file())
        self.assertTrue((SIMULATOR_ROOT / "ARCHITECTURE.md").is_file())

    def test_desktop_sources_do_not_include_device_headers(self) -> None:
        forbidden = (
            "esp_err.h",
            "freertos/",
            "bsp/",
            "driver/",
            "nvs_flash",
        )
        sources = list((SIMULATOR_ROOT / "src").glob("*.cpp"))
        sources.extend((SIMULATOR_ROOT / "include").rglob("*.hpp"))
        self.assertGreater(len(sources), 0)
        for source in sources:
            text = source.read_text()
            for value in forbidden:
                self.assertNotIn(value, text, source)

    def test_command_output_contract_is_private_and_versioned(self) -> None:
        implementation = (SIMULATOR_ROOT / "src" / "simulator.cpp").read_text()
        self.assertIn("kCommandProtocolVersion", implementation)
        status_start = implementation.index("std::string Simulator::status_json")
        status_end = implementation.index("bool Simulator::render_svg", status_start)
        status_code = implementation[status_start:status_end]
        self.assertNotIn("transcript_.data", status_code)
        self.assertNotIn("answer_.data", status_code)
        self.assertIn("transcript_bytes", status_code)
        self.assertIn("answer_bytes", status_code)

    def test_private_text_limits_match_the_agent_contract(self) -> None:
        simulator_header = (
            SIMULATOR_ROOT
            / "include"
            / "chatesp"
            / "simulator"
            / "simulator.hpp"
        ).read_text()
        agent_limits = (
            REPOSITORY_ROOT
            / "firmware"
            / "lib"
            / "agent_core"
            / "include"
            / "chatesp"
            / "agent_limits.hpp"
        ).read_text()
        self.assertIn("kMaximumTranscriptBytes = 2'048", simulator_header)
        self.assertIn("kMaximumAnswerBytes = 1'280", simulator_header)
        self.assertIn("max_transcript_bytes = 2'048", agent_limits)
        self.assertIn("max_answer_bytes = 1'280", agent_limits)

    def test_scenarios_do_not_contain_credential_fields(self) -> None:
        forbidden = (
            "api_key",
            "openrouter_key",
            "brave_key",
            "wifi_password",
            "authorization:",
        )
        scenarios = list((SIMULATOR_ROOT / "scenarios").glob("*.sim"))
        self.assertGreater(len(scenarios), 0)
        for scenario in scenarios:
            text = scenario.read_text().lower()
            for value in forbidden:
                self.assertNotIn(value, text, scenario)


if __name__ == "__main__":
    unittest.main()
