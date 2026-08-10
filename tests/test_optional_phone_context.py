from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "firmware" / "main" / "voice_runtime.cpp"
PROVIDER = ROOT / "firmware" / "main" / "network_context_provider.cpp"


class OptionalPhoneContextTests(unittest.TestCase):
    def test_voice_runtime_does_not_require_the_iphone_app(self) -> None:
        source = RUNTIME.read_text(encoding="utf-8")
        self.assertNotIn("SET UP THE WATCH", source)
        self.assertIn('fail("WI-FI IS NOT CONFIGURED")', source)
        self.assertIn(
            'fail("THE SERVICE KEY IS NOT CONFIGURED")', source
        )

    def test_time_and_location_fallbacks_are_bounded(self) -> None:
        runtime = RUNTIME.read_text(encoding="utf-8")
        provider = PROVIDER.read_text(encoding="utf-8")
        self.assertIn('kNtpServer[] = "time.cloudflare.com"', runtime)
        self.assertIn("1'500, 1'500, 1'000, 3'000, 1", provider)
        self.assertIn(
            "fields=success,city,region,country_code,timezone.offset",
            provider,
        )
        self.assertNotIn("latitude", provider)
        self.assertNotIn("longitude", provider)

    def test_ble_waits_for_the_ip_context_worker(self) -> None:
        source = RUNTIME.read_text(encoding="utf-8")
        self.assertIn(
            "!ble_start_attempted_ &&\n"
            "                    network_context_task_ == nullptr &&\n"
            "                    now_ms - network_context_finished_at_ms_ >=",
            source,
        )
        ensure = source[
            source.index("bool ensure_ble_started()") :
            source.index("bool reserve_ble_restart_memory()")
        ]
        self.assertIn("cancel_network_context_worker();", ensure)
        self.assertIn("network_.shutdown();", ensure)


if __name__ == "__main__":
    unittest.main()
