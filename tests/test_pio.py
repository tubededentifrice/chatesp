from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from tools.pio import (
    canonical_platformio_environment,
    dependency_check_is_fresh,
    dependency_policy_digest,
    device_write_policy_errors,
    first_party_write_api_errors,
    invocation_policy_errors,
    remove_aliased_watch_builds,
    prepare_profile_sdkconfigs,
    profile_sdkconfig_text,
    requested_watch_environments,
    requires_idf_python,
    selected_environments,
)


class PlatformioWrapperTests(unittest.TestCase):
    def test_platformio_uses_canonical_project_and_core_paths(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            container = Path(directory)
            root = container / "physical" / "repo"
            project = root / "firmware"
            project.mkdir(parents=True)
            alias = container / "alias"
            alias.symlink_to(root.parent, target_is_directory=True)

            environment = canonical_platformio_environment(
                {
                    "PWD": str(alias / "repo"),
                    "PLATFORMIO_CORE_DIR": str(
                        alias / "repo" / ".platformio"
                    ),
                },
                root,
                project,
            )

            self.assertEqual(str(project.resolve()), environment["PWD"])
            self.assertEqual(
                str((root / ".platformio").resolve()),
                environment["PLATFORMIO_CORE_DIR"],
            )

    def test_aliased_watch_build_data_is_removed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            container = Path(directory)
            root = container / "physical" / "repo"
            project = root / "firmware"
            build = project / ".pio" / "build" / "watch_dev"
            build.mkdir(parents=True)
            alias = container / "alias"
            alias.symlink_to(root.parent, target_is_directory=True)
            (build / "project_description.json").write_text(
                json.dumps(
                    {"sources": [str(alias / "repo" / "source.cpp")]}
                ),
                encoding="utf-8",
            )

            removed = remove_aliased_watch_builds(
                project, root, ["run", "-e", "watch_dev"]
            )

            self.assertEqual(["watch_dev"], removed)
            self.assertFalse(build.exists())

    def test_canonical_watch_build_data_is_kept(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve() / "repo"
            project = root / "firmware"
            build = project / ".pio" / "build" / "watch_dev"
            build.mkdir(parents=True)
            (build / "project_description.json").write_text(
                json.dumps({"sources": [str(root / "source.cpp")]}),
                encoding="utf-8",
            )

            removed = remove_aliased_watch_builds(
                project, root, ["run", "-e", "watch_dev"]
            )

            self.assertEqual([], removed)
            self.assertTrue(build.exists())

    def test_device_profiles_forbid_irreversible_writes(self) -> None:
        root = Path(__file__).resolve().parents[1]
        self.assertEqual(
            [], device_write_policy_errors(root / "firmware")
        )

    def test_device_profiles_persist_ble_bonds(self) -> None:
        root = Path(__file__).resolve().parents[1]
        for profile in ("watch_dev", "watch_prod"):
            defaults = (
                root / "firmware" / "config" / f"{profile}.defaults"
            ).read_text(encoding="utf-8")
            self.assertIn("CONFIG_BT_NIMBLE_NVS_PERSIST=y", defaults)

    def test_common_config_sizes_nimble_host_for_memory_ble(self) -> None:
        root = Path(__file__).resolve().parents[1]
        defaults = (root / "firmware" / "sdkconfig.defaults").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            "CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=8192", defaults
        )
        self.assertIn("CONFIG_BT_NIMBLE_ENABLE_CONN_REATTEMPT=n", defaults)

    def test_power_key_policy_uses_the_axp_long_hold_register(self) -> None:
        root = Path(__file__).resolve().parents[1]
        source = (
            root / "firmware" / "main" / "power_control.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("kAxp2101PowerOffEnable = 0x22", source)
        self.assertIn("kAxp2101LongHoldShutdown = 1U << 1", source)
        self.assertIn("repair_legacy_power_key_policy", source)
        self.assertIn("set_hardware_hold_shutdown(!pressed)", source)

    def test_production_system_off_discharges_rails_and_stays_latched(
        self,
    ) -> None:
        root = Path(__file__).resolve().parents[1]
        power_source = (
            root / "firmware" / "main" / "power_control.cpp"
        ).read_text(encoding="utf-8")
        app_source = (
            root / "firmware" / "main" / "app_main.cpp"
        ).read_text(encoding="utf-8")

        self.assertIn("kAxp2101InternalOffDischarge = 1U << 5", power_source)
        self.assertIn(
            "kAxp2101InternalOffDischarge | kAxp2101SoftwareOff",
            power_source,
        )
        wait_start = app_source.index("void wait_for_system_off_or_wake(")
        wait_end = app_source.index("}  // namespace", wait_start)
        wait_source = app_source[wait_start:wait_end]
        self.assertIn("while (runtime.poweroff_ready())", wait_source)
        self.assertIn("now_ms - last_request_at_ms", wait_source)
        self.assertIn("chatesp::power::power_off()", wait_source)
        self.assertIn("runtime.action_button_edge(true, now_ms)", wait_source)
        self.assertNotIn("runtime.poweroff_failed()", wait_source)
        self.assertNotIn("System off did not remove power", app_source)

    def test_development_sleep_keeps_the_panel_ready(self) -> None:
        root = Path(__file__).resolve().parents[1]
        runtime = (
            root / "firmware" / "main" / "voice_runtime.cpp"
        ).read_text(encoding="utf-8")
        ui = (root / "firmware" / "main" / "ui.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("display_sleep_result = ui::sleep(true)", runtime)
        self.assertIn("if (kDevelopmentMode)", runtime)
        self.assertIn("sleep_overlay", ui)
        self.assertIn("keep_panel_ready\n        ? ESP_OK", ui)

    def test_ble_advertising_has_shutdown_guard_and_recovery(self) -> None:
        root = Path(__file__).resolve().parents[1]
        ble = (
            root / "firmware" / "main" / "ble_provisioning.cpp"
        ).read_text(encoding="utf-8")
        runtime = (
            root / "firmware" / "main" / "voice_runtime.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("kAdvertiseRetryLimit = 5", ble)
        self.assertIn("schedule_advertise_retry", ble)
        self.assertIn("if (!s_stop_gate.running())", ble)
        self.assertIn("BLE advertising retries ended", ble)
        self.assertIn("advertising_recovery_requested", runtime)

    def test_common_config_bounds_wifi_ram_for_secure_pairing(self) -> None:
        root = Path(__file__).resolve().parents[1]
        defaults = (root / "firmware" / "sdkconfig.defaults").read_text(
            encoding="utf-8"
        )
        self.assertIn("CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=6", defaults)
        self.assertIn("CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=16", defaults)
        self.assertIn("CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=16", defaults)
        self.assertIn("CONFIG_ESP_WIFI_RX_BA_WIN=6", defaults)
        self.assertIn("CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=512", defaults)

    def test_ble_bond_requests_identity_keys_for_private_addresses(self) -> None:
        root = Path(__file__).resolve().parents[1]
        source = (
            root / "firmware" / "main" / "ble_provisioning.cpp"
        ).read_text(encoding="utf-8")
        self.assertGreaterEqual(
            source.count(
                "BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID"
            ),
            2,
        )
        self.assertIn("peer_identity", source)
        self.assertIn("Removed one stale BLE bond", source)

    def test_ble_idle_and_voice_radio_order_protects_restart_memory(self) -> None:
        root = Path(__file__).resolve().parents[1]
        source = (
            root / "firmware" / "main" / "voice_runtime.cpp"
        ).read_text(encoding="utf-8")
        run_start = source.index("void run()")
        loop_start = source.index("while (true)", run_start)
        startup_source = source[run_start:loop_start]
        self.assertIn("ensure_ble_started();", startup_source)
        self.assertNotIn("start_network_early", startup_source)
        warm_start = source.index("void start_network_during_recording()")
        warm_end = source.index("void run_network_warm_worker()", warm_start)
        warm_source = source[warm_start:warm_end]
        self.assertLess(
            warm_source.index("kPhoneProxyConnectGraceMs"),
            warm_source.index("stop_ble_for_request()"),
        )
        self.assertLess(
            warm_source.index("stop_ble_for_request()"),
            warm_source.index("xTaskCreatePinnedToCore("),
        )
        self.assertIn("ble_provisioning::bond_available()", warm_source)
        self.assertIn("runtime::keep_ble_during_recording(", warm_source)
        self.assertIn("proxy_wait_started_ms = monotonic_ms()", source)
        wake_start = source.index("void wake_for_button(")
        wake_end = source.index("void request_display_wake(", wake_start)
        self.assertIn("ensure_ble_started()", source[wake_start:wake_end])
        self.assertIn("ble_started_ || ble_provisioning::running()", source)
        context_start = source.index("void start_network_context_lookup()")
        context_end = source.index("void run_network_context_worker()")
        self.assertNotIn("stop_ble()", source[context_start:context_end])
        ble_start = source.index("bool ensure_ble_started()")
        ble_start_end = source.index("void recover_poweroff", ble_start)
        ble_start_source = source[ble_start:ble_start_end]
        self.assertIn("network_.shutdown()", ble_start_source)
        self.assertIn("kBleControllerMinimumLargestBlockBytes", ble_start_source)
        self.assertIn("kBleControllerMinimumFreeInternalBytes", ble_start_source)
        self.assertIn("reserve_ble_restart_memory()", source)
        self.assertIn("release_ble_restart_memory();", ble_start_source)
        self.assertIn("ble_memory_recovery_restart", source)
        self.assertIn("esp_restart();", source)
        network_source = (
            root / "firmware" / "main" / "network_manager.cpp"
        ).read_text(encoding="utf-8")
        shutdown_start = network_source.index("void NetworkManager::shutdown()")
        shutdown_end = network_source.index(
            "NetworkState NetworkManager::state()", shutdown_start
        )
        shutdown_source = network_source[shutdown_start:shutdown_end]
        self.assertIn("clean_failed_initialize();", shutdown_source)
        self.assertIn("initialized_ = false;", shutdown_source)

    def test_phone_proxy_is_secure_fast_and_preferred(self) -> None:
        root = Path(__file__).resolve().parents[1]
        ble_source = (
            root / "firmware" / "main" / "ble_provisioning.cpp"
        ).read_text(encoding="utf-8")
        transport_source = (
            root / "firmware" / "main" / "http_transport.cpp"
        ).read_text(encoding="utf-8")
        provider_source = (
            root / "firmware" / "main" / "cloud_providers.cpp"
        ).read_text(encoding="utf-8")
        ios_protocol = (
            root
            / "ios"
            / "ChatESP"
            / "Provisioning"
            / "ProvisioningProtocol.swift"
        ).read_text(encoding="utf-8")
        ios_ble = (
            root
            / "ios"
            / "ChatESP"
            / "Provisioning"
            / "BLEProvisioner.swift"
        ).read_text(encoding="utf-8")

        self.assertIn("BLE_GATT_CHR_F_NOTIFY |", ble_source)
        self.assertIn("BLE_GATT_CHR_F_WRITE_NO_RSP", ble_source)
        self.assertIn("BLE_GATT_CHR_F_WRITE_ENC", ble_source)
        self.assertIn("BLE_GATT_CHR_F_WRITE_AUTHEN", ble_source)
        self.assertIn("ble_gatts_notify_custom", ble_source)
        self.assertNotIn("s_http_proxy_indication_done", ble_source)
        self.assertIn(
            "if (ble_provisioning::http_proxy_available())",
            transport_source,
        )
        self.assertIn("encoded_length >", transport_source)
        self.assertNotIn("ble_svc_gatt_changed", ble_source)
        self.assertIn("transport.proxy_available()", provider_source)
        self.assertIn("7B2E2100-6F3C-4B8A-9D71-4C4553500001", ios_protocol)
        self.assertIn("return .withoutResponse", ios_ble)
        self.assertIn("return .withResponse", ios_ble)
        self.assertIn("phoneProxyWaitingForWriteResponse", ios_ble)
        self.assertIn("canSendWriteWithoutResponse", ios_ble)
        self.assertIn("session.bytes(", ios_ble)
        value_callback_start = ios_ble.index(
            "didUpdateValueFor characteristic: CBCharacteristic"
        )
        value_callback_end = ios_ble.index(
            "extension BLEProvisioner: CLLocationManagerDelegate",
            value_callback_start,
        )
        value_callback = ios_ble[value_callback_start:value_callback_end]
        self.assertIn("MainActor.assumeIsolated", value_callback)
        self.assertNotIn("Task { @MainActor", value_callback)

    def test_ios_reuses_discovered_settings_characteristics(self) -> None:
        root = Path(__file__).resolve().parents[1]
        source = (
            root / "ios" / "ChatESP" / "Provisioning" / "BLEProvisioner.swift"
        ).read_text(encoding="utf-8")
        provision_start = source.index("func provision(")
        prepare_start = source.index(
            "private func prepareCharacteristics", provision_start
        )
        provision_source = source[provision_start:prepare_start]
        self.assertIn("acknowledgementCharacteristic != nil", provision_source)
        self.assertIn("beginAttempt()", provision_source)
        self.assertIn("if selected === peripheral", source)
        self.assertIn("self.selected == nil", source)
        self.assertIn("let identifier = self.desiredSelectedID", source)

    def test_development_display_sleep_keeps_co5300_controller_on(self) -> None:
        root = Path(__file__).resolve().parents[1]
        board_source = (
            root
            / "firmware"
            / "components"
            / "chatesp_board"
            / "esp32_s3_touch_amoled_1_8.c"
        ).read_text(encoding="utf-8")
        start = board_source.index("esp_err_t bsp_display_backlight_off")
        end = board_source.index(
            "esp_err_t bsp_display_backlight_on", start
        )
        sleep_source = board_source[start:end]
        self.assertIn("return bsp_display_brightness_set(0);", sleep_source)
        self.assertNotIn("esp_lcd_panel_disp_on_off", sleep_source)
        self.assertIn("panel_brightness_is_zero", board_source)
        self.assertIn("waking_from_zero", board_source)

    def test_device_policy_rejects_an_unsafe_profile(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            (project / "config").mkdir()
            (project / "platformio.ini").write_text(
                "[env:watch_dev]\n"
                "build_flags=-DCHATESP_ALLOW_IRREVERSIBLE_DEVICE_WRITES=0\n"
                "board_build.cmake_extra_args="
                "-DCHATESP_PERMANENT_WRITE_POLICY=FORBID\n"
                "[env:watch_prod]\n"
                "build_flags="
                "-DCHATESP_ALLOW_IRREVERSIBLE_DEVICE_WRITES=0 "
                "-DCHATESP_ALLOW_IRREVERSIBLE_DEVICE_WRITES=1\n"
                "board_build.cmake_extra_args="
                "-DCHATESP_PERMANENT_WRITE_POLICY=ALLOW\n",
                encoding="utf-8",
            )
            safe_values = (
                "CONFIG_NVS_ENCRYPTION=n\n"
                "CONFIG_SECURE_BOOT=n\n"
                "CONFIG_SECURE_FLASH_ENC_ENABLED=n\n"
                "CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=n\n"
                "CONFIG_BOOTLOADER_ANTI_ROLLBACK_ENABLE=n\n"
                "CONFIG_SECURE_FLASH_PSEUDO_ROUND_FUNC=n\n"
                "CONFIG_SECURE_DISABLE_ROM_DL_MODE=n\n"
                "CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE=n\n"
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
                "watch_prod must set the zero irreversible-write flag exactly once",
                errors,
            )
            self.assertIn(
                "watch_prod must set the CMake permanent-write lock exactly once",
                errors,
            )
            self.assertIn(
                "watch_prod must set CONFIG_NVS_ENCRYPTION=n", errors
            )

    def test_watch_invocation_rejects_project_and_flag_overrides(self) -> None:
        errors = invocation_policy_errors(
            ["run", "-e", "watch_dev", "--project-dir=/tmp/other"],
            {"PLATFORMIO_BUILD_FLAGS": "-DUNSAFE=1"},
        )

        self.assertIn(
            "ChatESP device builds forbid the project override option: --project-dir",
            errors,
        )
        self.assertIn(
            "ChatESP device builds forbid the project override variable: "
            "PLATFORMIO_BUILD_FLAGS",
            errors,
        )

    def test_unknown_environment_is_rejected(self) -> None:
        self.assertEqual(
            ["unknown PlatformIO environment: watch_unsafe"],
            invocation_policy_errors(
                ["run", "-e", "watch_unsafe"], {}
            ),
        )

    def test_first_party_efuse_write_api_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            source = project / "main" / "unsafe.cpp"
            source.parent.mkdir()
            source.write_text(
                "void unsafe() { esp_efuse_write_field_blob(); }\n",
                encoding="utf-8",
            )

            self.assertEqual(
                [
                    "main/unsafe.cpp uses a forbidden eFuse source marker: "
                    "esp_efuse_"
                ],
                first_party_write_api_errors(project),
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
