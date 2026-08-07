#pragma once

#include <cstdint>

#include "esp_err.h"

namespace chatesp {

class SettingsStore;

namespace ble_provisioning {

using PasskeyCallback = void (*)(
    std::uint32_t passkey, bool visible, void *context);

esp_err_t start(
    SettingsStore *settings_store,
    PasskeyCallback passkey_callback,
    void *callback_context);
esp_err_t stop();
[[nodiscard]] bool running();

}  // namespace ble_provisioning
}  // namespace chatesp
