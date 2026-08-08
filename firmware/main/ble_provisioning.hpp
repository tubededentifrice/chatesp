#pragma once

#include <cstdint>
#include <cstddef>

#include "esp_err.h"

namespace chatesp {

class SettingsStore;

namespace ble_provisioning {

using PasskeyCallback = void (*)(
    std::uint32_t passkey, bool visible, void *context);
using DeviceContextCallback = void (*)(
    std::uint64_t epoch_seconds,
    std::int16_t utc_offset_minutes,
    const char *approximate_location,
    std::size_t approximate_location_size,
    void *context);

esp_err_t start(
    SettingsStore *settings_store,
    PasskeyCallback passkey_callback,
    DeviceContextCallback device_context_callback,
    void *callback_context);
esp_err_t stop();
[[nodiscard]] bool running();

}  // namespace ble_provisioning
}  // namespace chatesp
