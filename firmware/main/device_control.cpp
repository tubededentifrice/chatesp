#include "device_control.hpp"

#include "power_control.hpp"
#include "ui.hpp"

namespace chatesp {

DeviceControl::DeviceControl(
    DevicePreferencesStore &store,
    const runtime::DevicePreferences &initial,
    bool development_mode)
    : store_(store),
      brightness_percent_(initial.brightness_percent),
      volume_percent_(initial.volume_percent),
      values_persisted_(store.persistent()),
      development_mode_(development_mode) {}

agent::Error DeviceControl::status(agent::DeviceStatus &status) {
    status.brightness_percent = brightness_percent();
    status.volume_percent = volume_percent();
    const auto battery = power::battery_percent();
    status.battery_available = battery.has_value();
    status.battery_percent = battery.value_or(0);
    status.settings_persistent =
        values_persisted_.load(std::memory_order_acquire);
    status.power_off_mode = development_mode_
        ? agent::PowerOffMode::development_sleep
        : agent::PowerOffMode::system_off;
    return agent::Error::none;
}

agent::Error DeviceControl::set_brightness(
    std::uint8_t percent, bool &persisted) {
    const agent::Error error = preview_brightness(percent);
    if (error != agent::Error::none) {
        return error;
    }
    persisted = persist_preferences();
    return agent::Error::none;
}

agent::Error DeviceControl::preview_brightness(std::uint8_t percent) {
    if (percent < runtime::DevicePreferences::minimum_brightness_percent ||
        percent > runtime::DevicePreferences::maximum_percent) {
        return agent::Error::invalid_argument;
    }
    if (ui::set_brightness(percent) != ESP_OK) {
        return agent::Error::tool_failed;
    }
    brightness_percent_.store(percent, std::memory_order_release);
    return agent::Error::none;
}

agent::Error DeviceControl::set_volume(
    std::uint8_t percent, bool &persisted) {
    const agent::Error error = preview_volume(percent);
    if (error != agent::Error::none) {
        return error;
    }
    persisted = persist_preferences();
    return agent::Error::none;
}

agent::Error DeviceControl::preview_volume(std::uint8_t percent) {
    if (percent > runtime::DevicePreferences::maximum_percent) {
        return agent::Error::invalid_argument;
    }
    volume_percent_.store(percent, std::memory_order_release);
    return agent::Error::none;
}

bool DeviceControl::persist_preferences() {
    const bool persisted = store_.store(current_preferences());
    values_persisted_.store(persisted, std::memory_order_release);
    return persisted;
}

agent::Error DeviceControl::schedule_power_off(agent::PowerOffMode &mode) {
    mode = development_mode_
        ? agent::PowerOffMode::development_sleep
        : agent::PowerOffMode::system_off;
    power_off_pending_.store(true, std::memory_order_release);
    return agent::Error::none;
}

runtime::DevicePreferences DeviceControl::current_preferences() const {
    return {brightness_percent(), volume_percent()};
}

}  // namespace chatesp
