#pragma once

#include <atomic>
#include <cstdint>

#include "chatesp/agent_interfaces.hpp"
#include "chatesp/device_preferences.hpp"
#include "device_preferences_store.hpp"

namespace chatesp {

enum class PendingDeviceAction : std::uint8_t {
    none,
    power_off,
    restart,
};

class DeviceControl final : public agent::DeviceControlProvider {
public:
    DeviceControl(
        DevicePreferencesStore &store,
        const runtime::DevicePreferences &initial,
        bool development_mode);

    agent::Error status(agent::DeviceStatus &status) override;
    agent::Error set_brightness(
        std::uint8_t percent, bool &persisted) override;
    agent::Error set_volume(
        std::uint8_t percent, bool &persisted) override;
    agent::Error schedule_power_off(agent::PowerOffMode &mode) override;
    agent::Error schedule_restart() override;

    // Apply touch-control previews without a flash write. Persist once after
    // the user releases the active control.
    agent::Error preview_brightness(std::uint8_t percent);
    agent::Error preview_volume(std::uint8_t percent);
    bool persist_preferences();

    [[nodiscard]] std::uint8_t brightness_percent() const {
        return brightness_percent_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint8_t volume_percent() const {
        return volume_percent_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool power_off_pending() const {
        return pending_action_.load(std::memory_order_acquire) ==
            PendingDeviceAction::power_off;
    }
    [[nodiscard]] bool restart_pending() const {
        return pending_action_.load(std::memory_order_acquire) ==
            PendingDeviceAction::restart;
    }
    void cancel_pending_action() {
        pending_action_.store(
            PendingDeviceAction::none, std::memory_order_release);
    }

private:
    [[nodiscard]] runtime::DevicePreferences current_preferences() const;

    DevicePreferencesStore &store_;
    std::atomic<std::uint8_t> brightness_percent_;
    std::atomic<std::uint8_t> volume_percent_;
    std::atomic<bool> values_persisted_;
    std::atomic<PendingDeviceAction> pending_action_{
        PendingDeviceAction::none};
    bool development_mode_ = false;
};

}  // namespace chatesp
