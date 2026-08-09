#pragma once

#include <cstdint>

#include "esp_err.h"

namespace chatesp {

class DevicePreferencesStore;
class DeviceMemoryStore;

class VoiceRuntime {
public:
    VoiceRuntime();
    ~VoiceRuntime();

    VoiceRuntime(const VoiceRuntime &) = delete;
    VoiceRuntime &operator=(const VoiceRuntime &) = delete;

    esp_err_t start(
        bool startup_button_down, std::uint32_t startup_at_ms,
        DevicePreferencesStore &device_preferences_store,
        DeviceMemoryStore &device_memory_store);
    void action_button_edge(
        bool pressed, std::uint32_t at_ms,
        bool short_press_confirmed = false);
    [[nodiscard]] bool mode_button_available() const;
    void mode_button_edge(bool pressed);
    void mode_button_short_press(std::uint32_t at_ms);

    [[nodiscard]] bool poweroff_ready() const;
    void poweroff_failed();

private:
    class Impl;
    Impl *impl_ = nullptr;
};

}  // namespace chatesp
