#include "device_settings.hpp"

#ifndef CHATESP_DEVELOPMENT_MODE
#define CHATESP_DEVELOPMENT_MODE 0
#endif

#if CHATESP_DEVELOPMENT_MODE && __has_include("local_config.h")
#include "local_config.h"
#define CHATESP_HAS_LOCAL_CONFIG 1
#else
#define CHATESP_HAS_LOCAL_CONFIG 0
#endif

namespace chatesp {

#if CHATESP_HAS_LOCAL_CONFIG
namespace {

static_assert(sizeof(CHATESP_LOCAL_CHAT_ENDPOINT) - 1 >= 12);
static_assert(
    sizeof(CHATESP_LOCAL_CHAT_ENDPOINT) - 1 <=
    DeviceSettingsView::kMaxChatEndpointBytes);
static_assert(sizeof(CHATESP_LOCAL_OPENROUTER_API_KEY) - 1 >= 8);
static_assert(
    sizeof(CHATESP_LOCAL_OPENROUTER_API_KEY) - 1 <=
    DeviceSettingsView::kMaxOpenRouterKeyBytes);
static_assert(
    sizeof(CHATESP_LOCAL_BRAVE_API_KEY) - 1 <=
    DeviceSettingsView::kMaxBraveKeyBytes);
static_assert(sizeof(CHATESP_LOCAL_WIFI_SSID) - 1 >= 1);
static_assert(
    sizeof(CHATESP_LOCAL_WIFI_SSID) - 1 <=
    DeviceSettingsView::kMaxWifiSsidBytes);
static_assert(sizeof(CHATESP_LOCAL_WIFI_PASSWORD) - 1 >= 8);
static_assert(
    sizeof(CHATESP_LOCAL_WIFI_PASSWORD) - 1 <=
    DeviceSettingsView::kMaxWifiPasswordBytes);

}  // namespace
#endif

DeviceSettingsView device_settings() noexcept {
#if CHATESP_HAS_LOCAL_CONFIG
    return {
        CHATESP_LOCAL_CHAT_ENDPOINT,
        CHATESP_LOCAL_OPENROUTER_API_KEY,
        CHATESP_LOCAL_BRAVE_API_KEY,
        CHATESP_LOCAL_WIFI_SSID,
        CHATESP_LOCAL_WIFI_PASSWORD,
        true,
    };
#else
    return {};
#endif
}

}  // namespace chatesp
