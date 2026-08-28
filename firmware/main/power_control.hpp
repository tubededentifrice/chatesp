#pragma once

#include <cstdint>
#include <optional>

#include "chatesp/battery_status.hpp"
#include "chatesp/power_button_filter.hpp"
#include "esp_err.h"

namespace chatesp::power {

esp_err_t initialize();
esp_err_t poll(std::uint32_t now_ms, ButtonEdges *edges);
esp_err_t poll_mode_button(std::uint32_t now_ms, ButtonEdges *edges);
bool action_button_is_pressed();
bool mode_button_is_pressed();
[[nodiscard]] bool startup_power_key_wake_confirmed();
[[nodiscard]] std::uint32_t startup_button_started_at_ms(
    std::uint32_t observed_at_ms);
[[nodiscard]] std::optional<BatteryStatus> battery_status();
[[nodiscard]] std::optional<std::uint8_t> battery_percent();
[[nodiscard]] std::uint16_t battery_sample_failure_count();
[[nodiscard]] std::uint32_t i2c_error_count();
[[nodiscard]] std::uint32_t power_source_revision();
esp_err_t power_off();

}  // namespace chatesp::power
