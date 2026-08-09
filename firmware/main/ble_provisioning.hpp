#pragma once

#include <cstdint>
#include <cstddef>

#include "esp_err.h"

namespace chatesp {

class SettingsStore;
class DeviceMemoryStore;

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
    DeviceMemoryStore *memory_store,
    PasskeyCallback passkey_callback,
    DeviceContextCallback device_context_callback,
    void *callback_context);
esp_err_t stop(std::uint32_t timeout_ms);
[[nodiscard]] bool running();
[[nodiscard]] bool bond_available();
[[nodiscard]] bool settings_confirmation_pending();
[[nodiscard]] bool advertising_recovery_requested();
[[nodiscard]] bool secure_link_connected();

constexpr std::size_t kMaximumHttpProxyFrameSize = 512;

[[nodiscard]] bool http_proxy_available();
[[nodiscard]] std::size_t http_proxy_frame_capacity();
void begin_http_proxy_exchange();
esp_err_t send_http_proxy_frame(
    const std::uint8_t *data, std::size_t size, std::uint32_t timeout_ms);
esp_err_t receive_http_proxy_frame(
    std::uint8_t *data, std::size_t capacity, std::size_t *size,
    std::uint32_t timeout_ms);
void cancel_http_proxy_exchange();

}  // namespace ble_provisioning
}  // namespace chatesp
