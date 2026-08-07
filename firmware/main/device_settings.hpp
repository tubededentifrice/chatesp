#pragma once

#include <cstddef>
#include <string_view>

namespace chatesp {

struct DeviceSettingsView {
    static constexpr std::size_t kMaxChatEndpointBytes = 192;
    static constexpr std::size_t kMaxOpenRouterKeyBytes = 256;
    static constexpr std::size_t kMaxBraveKeyBytes = 128;
    static constexpr std::size_t kMaxWifiSsidBytes = 32;
    static constexpr std::size_t kMaxWifiPasswordBytes = 63;

    std::string_view chat_endpoint;
    std::string_view openrouter_api_key;
    std::string_view brave_api_key;
    std::string_view wifi_ssid;
    std::string_view wifi_password;
    bool configured = false;
};

[[nodiscard]] DeviceSettingsView device_settings() noexcept;

}  // namespace chatesp
