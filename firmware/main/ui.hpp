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

struct QuickControlsUpdate {
    std::uint8_t brightness_percent = 65;
    std::uint8_t volume_percent = 70;
    bool commit = false;
};

using QuickControlsCallback = void (*)(
    const QuickControlsUpdate &update, void *context);

// Start the display and show the full-boot splash. Do not use this function
// for an in-session display wake.
bool start(std::uint8_t brightness_percent);

// Install the non-blocking control callback after the runtime can accept
// updates. The callback runs from the LVGL task and must not block.
bool enable_quick_controls(
    std::uint8_t brightness_percent,
    std::uint8_t volume_percent,
    QuickControlsCallback callback,
    void *context);
void disable_quick_controls();

// Update the visible values after the runtime applies or rejects an update.
// The caller must own the BSP display lock.
void sync_quick_controls(
    std::uint8_t brightness_percent, std::uint8_t volume_percent);

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
esp_err_t wake(
    InteractionState state, std::uint8_t brightness_percent);
esp_err_t reassert_panel(std::uint8_t brightness_percent);
esp_err_t set_brightness(std::uint8_t brightness_percent);

}  // namespace chatesp::ui
