#include "network_manager.hpp"

#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

namespace chatesp {
namespace network {
namespace {

constexpr char kTag[] = "network";
constexpr EventBits_t kConnected = BIT0;
constexpr EventBits_t kDisconnected = BIT1;
constexpr std::uint32_t kWaitSliceMs = 100;

std::uint32_t monotonic_ms() {
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1'000ULL);
}

bool elapsed(
    std::uint32_t start_ms, std::uint32_t now_ms, std::uint32_t limit_ms) {
    return static_cast<std::uint32_t>(now_ms - start_ms) >= limit_ms;
}

bool valid_credentials(const WifiCredentials &credentials) {
    return credentials.ssid != nullptr && credentials.ssid_size > 0 &&
           credentials.ssid_size <= 32 &&
           credentials.password_size <= 64 &&
           (credentials.password_size == 0 || credentials.password != nullptr) &&
           std::memchr(credentials.ssid, '\0', credentials.ssid_size) == nullptr &&
           (credentials.password_size == 0 ||
            std::memchr(
                credentials.password, '\0', credentials.password_size) ==
                nullptr);
}

bool valid_policy(const ConnectPolicy &policy) {
    return policy.attempt_timeout_ms > 0 && policy.total_timeout_ms > 0 &&
           policy.max_attempts > 0 && policy.max_attempts <= 5 &&
           policy.retry_delay_ms <= 5'000;
}

bool authentication_failure(std::uint8_t reason) {
    return reason == WIFI_REASON_AUTH_EXPIRE ||
           reason == WIFI_REASON_AUTH_FAIL ||
           reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
}

void wipe(void *data, std::size_t size) {
    auto *bytes = static_cast<volatile std::uint8_t *>(data);
    while (size-- != 0) {
        *bytes++ = 0;
    }
}

}  // namespace

esp_err_t NetworkManager::initialize() {
    if (initialized_) {
        return ESP_OK;
    }
    esp_err_t error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }
    events_ = xEventGroupCreate();
    if (events_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    station_ = esp_netif_create_default_wifi_sta();
    if (station_ == nullptr) {
        vEventGroupDelete(events_);
        events_ = nullptr;
        return ESP_FAIL;
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    error = esp_wifi_init(&init);
    if (error != ESP_OK) {
        clean_failed_initialize();
        return error;
    }
    wifi_driver_initialized_ = true;
    error = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, this, &wifi_events_);
    if (error != ESP_OK) {
        clean_failed_initialize();
        return error;
    }
    error = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, this, &ip_events_);
    if (error != ESP_OK) {
        clean_failed_initialize();
        return error;
    }
    if ((error = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (error = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
        (error = esp_wifi_start()) != ESP_OK) {
        clean_failed_initialize();
        return error;
    }
    wifi_started_ = true;
    initialized_ = true;
    ESP_LOGI(kTag, "Wi-Fi is ready");
    return ESP_OK;
}

agent::Error NetworkManager::connect(
    const WifiCredentials &credentials,
    agent::CancellationToken &cancellation, const ConnectPolicy &policy) {
    if (!initialized_ || !valid_credentials(credentials) ||
        !valid_policy(policy)) {
        return agent::Error::invalid_argument;
    }
    if (!wifi_started_) {
        if (esp_wifi_start() != ESP_OK) {
            return agent::Error::disconnected;
        }
        wifi_started_ = true;
    }
    if (connected()) {
        return agent::Error::none;
    }
    wifi_config_t config{};
    std::memcpy(config.sta.ssid, credentials.ssid, credentials.ssid_size);
    if (credentials.password_size != 0) {
        std::memcpy(
            config.sta.password, credentials.password,
            credentials.password_size);
    }
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.threshold.authmode = credentials.password_size == 0
                                        ? WIFI_AUTH_OPEN
                                        : WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    const esp_err_t config_error = esp_wifi_set_config(WIFI_IF_STA, &config);
    wipe(&config, sizeof(config));
    if (config_error != ESP_OK) {
        return agent::Error::disconnected;
    }

    const std::uint32_t total_start = monotonic_ms();
    for (std::uint8_t attempt = 0; attempt < policy.max_attempts; ++attempt) {
        if (elapsed(
                total_start, monotonic_ms(), policy.total_timeout_ms)) {
            break;
        }
        if (cancellation.cancelled()) {
            disconnect();
            return agent::Error::cancelled;
        }
        xEventGroupClearBits(events_, kConnected | kDisconnected);
        last_disconnect_reason_.store(0, std::memory_order_release);
        if (esp_wifi_connect() != ESP_OK) {
            disconnect();
            return agent::Error::disconnected;
        }
        const std::uint32_t attempt_start = monotonic_ms();
        while (!elapsed(
                   attempt_start, monotonic_ms(), policy.attempt_timeout_ms) &&
               !elapsed(total_start, monotonic_ms(), policy.total_timeout_ms)) {
            if (cancellation.cancelled()) {
                disconnect();
                return agent::Error::cancelled;
            }
            const EventBits_t bits = xEventGroupWaitBits(
                events_, kConnected | kDisconnected, pdFALSE, pdFALSE,
                pdMS_TO_TICKS(kWaitSliceMs));
            if ((bits & kConnected) != 0) {
                ESP_LOGI(kTag, "Wi-Fi connected");
                return agent::Error::none;
            }
            if ((bits & kDisconnected) != 0 &&
                authentication_failure(
                    last_disconnect_reason_.load(std::memory_order_acquire))) {
                disconnect();
                return agent::Error::authentication;
            }
            if ((bits & kDisconnected) != 0) {
                xEventGroupClearBits(events_, kDisconnected);
                break;
            }
        }
        esp_wifi_disconnect();
        if (attempt + 1 < policy.max_attempts &&
            !elapsed(total_start, monotonic_ms(), policy.total_timeout_ms)) {
            std::uint32_t waited = 0;
            while (waited < policy.retry_delay_ms) {
                if (cancellation.cancelled()) {
                    disconnect();
                    return agent::Error::cancelled;
                }
                const std::uint32_t total_spent =
                    monotonic_ms() - total_start;
                if (total_spent >= policy.total_timeout_ms) {
                    break;
                }
                const std::uint32_t retry_remaining =
                    policy.retry_delay_ms - waited;
                const std::uint32_t total_remaining =
                    policy.total_timeout_ms - total_spent;
                std::uint32_t slice =
                    retry_remaining < kWaitSliceMs
                        ? retry_remaining
                        : kWaitSliceMs;
                if (slice > total_remaining) {
                    slice = total_remaining;
                }
                vTaskDelay(pdMS_TO_TICKS(slice));
                waited += slice;
            }
        }
    }
    disconnect();
    return agent::Error::connect_timeout;
}

void NetworkManager::disconnect() {
    if (!initialized_) {
        return;
    }
    esp_wifi_disconnect();
    xEventGroupClearBits(events_, kConnected | kDisconnected);
}

void NetworkManager::shutdown() {
    if (!wifi_started_) {
        return;
    }
    disconnect();
    esp_wifi_stop();
    wifi_started_ = false;
}

bool NetworkManager::connected() const {
    return initialized_ &&
           (xEventGroupGetBits(events_) & kConnected) != 0;
}

void NetworkManager::event_handler(
    void *argument, esp_event_base_t event_base, std::int32_t event_id,
    void *event_data) {
    if (argument != nullptr) {
        static_cast<NetworkManager *>(argument)->handle_event(
            event_base, event_id, event_data);
    }
}

void NetworkManager::handle_event(
    esp_event_base_t event_base, std::int32_t event_id, void *event_data) {
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(events_, kConnected);
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (event_data != nullptr) {
            last_disconnect_reason_.store(
                static_cast<wifi_event_sta_disconnected_t *>(event_data)->reason,
                std::memory_order_release);
        }
        xEventGroupClearBits(events_, kConnected);
        xEventGroupSetBits(events_, kDisconnected);
    }
}

void NetworkManager::clean_failed_initialize() {
    if (ip_events_ != nullptr) {
        esp_event_handler_instance_unregister(
            IP_EVENT, IP_EVENT_STA_GOT_IP, ip_events_);
        ip_events_ = nullptr;
    }
    if (wifi_events_ != nullptr) {
        esp_event_handler_instance_unregister(
            WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_events_);
        wifi_events_ = nullptr;
    }
    if (wifi_driver_initialized_) {
        esp_wifi_deinit();
        wifi_driver_initialized_ = false;
    }
    if (station_ != nullptr) {
        esp_netif_destroy_default_wifi(station_);
        station_ = nullptr;
    }
    if (events_ != nullptr) {
        vEventGroupDelete(events_);
        events_ = nullptr;
    }
}

}  // namespace network
}  // namespace chatesp
