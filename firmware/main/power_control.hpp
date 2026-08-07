#pragma once

#include <cstdint>

#include "chatesp/button_debouncer.hpp"
#include "esp_err.h"

namespace chatesp::power {

esp_err_t initialize();
esp_err_t poll(std::uint32_t now_ms, ButtonEdges *edges);
bool action_button_is_pressed();
esp_err_t power_off();

}  // namespace chatesp::power
