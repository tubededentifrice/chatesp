#pragma once

#include <cstdint>
#include <string_view>

#include "chatesp/agent_types.hpp"
#include "chatesp/app_mode.hpp"
#include "chatesp/audio_spectrum.hpp"
#include "chatesp/interaction_state.hpp"
#include "esp_err.h"
#include "image_frame.hpp"

namespace chatesp::ui {

enum class RadioIndicator : std::uint8_t {
    setup,
    off,
    wifi_connecting,
    wifi_online,
    ble_online,
    failed,
};

struct QuickControlsUpdate {
    std::uint8_t brightness_percent = 65;
    std::uint8_t volume_percent = 70;
    bool brightness_changed = false;
    bool volume_changed = false;
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

// The caller must own the BSP display lock for the clock and show functions.
// Clock style is a bounded value type so a later preferences record can supply
// it without a change to the drawing code.
[[nodiscard]] bool set_clock_style(const ClockStyle &style);
void show_app_mode(AppMode mode, InteractionState chat_state);
void show_clock_time(bool available, ClockTime time = {});

void show_state(InteractionState state);
void show_recording_spectrum(const AudioSpectrum &levels);
void show_transcript(std::string_view transcript);
void show_answer_stream(std::string_view answer);
void show_answer(std::string_view answer);
void show_answer_notice(std::string_view answer, std::string_view notice);
void show_error(std::string_view error);
void show_wifi_progress(std::string_view detail);
void show_model_progress(std::string_view detail);
void show_ble_passkey(std::uint32_t passkey, bool visible);
void show_footer(
    RadioIndicator radio, std::uint8_t signal_band, bool battery_available,
    std::uint8_t battery_percent, bool battery_charging);
[[nodiscard]] bool show_fullscreen_image(image::Rgb565Frame &&frame);
[[nodiscard]] bool show_fullscreen_plot(const agent::PlotData &plot);
void hide_fullscreen_image();
void hide_fullscreen_visual();

// A soft sleep draws a black frame but keeps the panel ready. A system-off
// sleep can also set brightness to zero after its cancel window has ended.
esp_err_t sleep(bool keep_panel_ready);
esp_err_t wake(
    InteractionState state, std::uint8_t brightness_percent,
    AppMode mode = AppMode::chat);
esp_err_t reassert_panel(std::uint8_t brightness_percent);
esp_err_t set_brightness(std::uint8_t brightness_percent);

}  // namespace chatesp::ui
