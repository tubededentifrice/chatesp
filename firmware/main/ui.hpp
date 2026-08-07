#pragma once

#include <cstdint>
#include <string_view>

#include "chatesp/interaction_state.hpp"
#include "esp_err.h"

namespace chatesp::ui {

bool start();

// The caller must own the BSP display lock for all show functions.
void show_state(InteractionState state);
void show_recording_level(std::uint8_t percent);
void show_transcript(std::string_view transcript);
void show_answer(std::string_view answer);
void show_error(std::string_view error);
void show_wifi_progress(std::string_view detail);
void show_model_progress(std::string_view detail);
void show_ble_passkey(std::uint32_t passkey, bool visible);

esp_err_t sleep();
esp_err_t wake(InteractionState state);

}  // namespace chatesp::ui
