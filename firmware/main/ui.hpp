#pragma once

#include <cstdint>
#include <string_view>

#include "chatesp/interaction_state.hpp"
#include "esp_err.h"
#include "image_frame.hpp"

namespace chatesp::ui {

enum class WifiIndicator : std::uint8_t {
    setup,
    off,
    connecting,
    online,
    failed,
};

bool start();

// The caller must own the BSP display lock for all show functions.
void show_state(InteractionState state);
void show_recording_level(std::uint8_t percent);
void show_transcript(std::string_view transcript);
void show_answer_stream(std::string_view answer);
void show_answer(std::string_view answer);
void show_answer_notice(std::string_view answer, std::string_view notice);
void show_error(std::string_view error);
void show_wifi_progress(std::string_view detail);
void show_model_progress(std::string_view detail);
void show_ble_passkey(std::uint32_t passkey, bool visible);
void show_footer(
    WifiIndicator wifi, bool battery_available,
    std::uint8_t battery_percent);
[[nodiscard]] bool show_fullscreen_image(image::Rgb565Frame &&frame);
void hide_fullscreen_image();

esp_err_t sleep();
esp_err_t wake(InteractionState state);
esp_err_t reassert_panel();

}  // namespace chatesp::ui
