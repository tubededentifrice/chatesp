#pragma once

#include <cstdint>
#include <optional>

#include "chatesp/power_button_filter.hpp"
#include "esp_err.h"

namespace chatesp::power {

esp_err_t initialize();
esp_err_t poll(std::uint32_t now_ms, ButtonEdges *edges);
esp_err_t poll_mode_button(std::uint32_t now_ms, ButtonEdges *edges);
bool action_button_is_pressed();
[[nodiscard]] std::optional<std::uint8_t> battery_percent();
esp_err_t power_off();

}  // namespace chatesp::power
