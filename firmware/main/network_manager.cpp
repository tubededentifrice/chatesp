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
constexpr std::uint32_t kDisconnectDrainMs = 100;
constexpr std::int8_t kMinimumUsefulRssiDbm = -75;
constexpr std::uint8_t kBestAccessPointRetryCount = 2;

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
        state_.store(NetworkState::failed, std::memory_order_release);
        return error;
    }
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        state_.store(NetworkState::failed, std::memory_order_release);
        return error;
    }
    events_ = xEventGroupCreate();
    if (events_ == nullptr) {
        state_.store(NetworkState::failed, std::memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    station_ = esp_netif_create_default_wifi_sta();
    if (station_ == nullptr) {
        vEventGroupDelete(events_);
        events_ = nullptr;
        state_.store(NetworkState::failed, std::memory_order_release);
        return ESP_FAIL;
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    error = esp_wifi_init(&init);
    if (error != ESP_OK) {
        clean_failed_initialize();
        state_.store(NetworkState::failed, std::memory_order_release);
        return error;
    }
    wifi_driver_initialized_ = true;
    error = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, this, &wifi_events_);
    if (error != ESP_OK) {
        clean_failed_initialize();
        state_.store(NetworkState::failed, std::memory_order_release);
        return error;
    }
    error = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, this, &ip_events_);
    if (error != ESP_OK) {
        clean_failed_initialize();
        state_.store(NetworkState::failed, std::memory_order_release);
        return error;
    }
    if ((error = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (error = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
        (error = esp_wifi_start()) != ESP_OK) {
        clean_failed_initialize();
        state_.store(NetworkState::failed, std::memory_order_release);
        return error;
    }
    wifi_started_ = true;
    const esp_err_t power_error = esp_wifi_set_max_tx_power(80);
    if (power_error != ESP_OK) {
        ESP_LOGW(
            kTag,
            "Wi-Fi maximum transmit power was not set (category %s)",
            esp_err_to_name(power_error));
    }
    initialized_ = true;
    state_.store(NetworkState::off, std::memory_order_release);
    ESP_LOGI(kTag, "Wi-Fi is ready");
    return ESP_OK;
}

agent::Error NetworkManager::apply_credentials(
    const WifiCredentials &credentials) {
    if (!initialized_ || !valid_credentials(credentials)) {
        return agent::Error::invalid_argument;
    }
    wifi_config_t config{};
    std::memcpy(config.sta.ssid, credentials.ssid, credentials.ssid_size);
    if (credentials.password_size != 0) {
        std::memcpy(
            config.sta.password, credentials.password,
            credentials.password_size);
    }
    // A fast scan can stop at the first matching SSID and select a distant
    // mesh node. Scan all 2.4 GHz channels once and select the strongest match.
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    // Do not move a live session to a distant mesh node. Retry the strongest
    // access point first, then let the normal 10-second idle retry rescan in
    // the background if no useful access point is available.
    config.sta.threshold.rssi = kMinimumUsefulRssiDbm;
    config.sta.failure_retry_cnt = kBestAccessPointRetryCount;
    config.sta.threshold.authmode = credentials.password_size == 0
                                        ? WIFI_AUTH_OPEN
                                        : WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    const esp_err_t config_error = esp_wifi_set_config(WIFI_IF_STA, &config);
    wipe(&config, sizeof(config));
    return config_error == ESP_OK ? agent::Error::none
                                  : agent::Error::disconnected;
}

agent::Error NetworkManager::ensure_wifi_started() {
    if (!initialized_) {
        return agent::Error::invalid_argument;
    }
    if (wifi_started_) {
        return agent::Error::none;
    }
    if (esp_wifi_start() != ESP_OK) {
        state_.store(NetworkState::failed, std::memory_order_release);
        return agent::Error::disconnected;
    }
    wifi_started_ = true;
    state_.store(NetworkState::off, std::memory_order_release);
    return agent::Error::none;
}

agent::Error NetworkManager::start_connect(
    const WifiCredentials &credentials) {
    if (connected() || connecting()) {
        return agent::Error::none;
    }
    const agent::Error start_error = ensure_wifi_started();
    if (start_error != agent::Error::none) {
        return start_error;
    }
    const agent::Error credentials_error = apply_credentials(credentials);
    if (credentials_error != agent::Error::none) {
        state_.store(NetworkState::failed, std::memory_order_release);
        return credentials_error;
    }
    if (connected() || connecting()) {
        return agent::Error::none;
    }
    xEventGroupClearBits(events_, kConnected | kDisconnected);
    last_disconnect_reason_.store(0, std::memory_order_release);
    connect_started_at_ms_.store(monotonic_ms(), std::memory_order_release);
    connect_started_.store(true, std::memory_order_release);
    state_.store(NetworkState::connecting, std::memory_order_release);
    if (esp_wifi_connect() != ESP_OK) {
        connect_started_.store(false, std::memory_order_release);
        state_.store(NetworkState::failed, std::memory_order_release);
        return agent::Error::disconnected;
    }
    return agent::Error::none;
}

agent::Error NetworkManager::connect(
    const WifiCredentials &credentials,
    agent::CancellationToken &cancellation, const ConnectPolicy &policy) {
    if (!initialized_ || !valid_credentials(credentials) ||
        !valid_policy(policy)) {
        return agent::Error::invalid_argument;
    }
    const agent::Error start_error = ensure_wifi_started();
    if (start_error != agent::Error::none) {
        return start_error;
    }
    if (connected()) {
        return agent::Error::none;
    }
    const std::uint32_t total_start = monotonic_ms();
    if (connect_started_.load(std::memory_order_acquire)) {
        const std::uint32_t pending_start =
            connect_started_at_ms_.load(std::memory_order_acquire);
        while (!elapsed(
                   pending_start, monotonic_ms(), policy.attempt_timeout_ms) &&
               !elapsed(total_start, monotonic_ms(), policy.total_timeout_ms)) {
            if (cancellation.cancelled()) {
                disconnect(false);
                return agent::Error::cancelled;
            }
            const EventBits_t bits = xEventGroupWaitBits(
                events_, kConnected | kDisconnected, pdFALSE, pdFALSE,
                pdMS_TO_TICKS(kWaitSliceMs));
            if ((bits & kConnected) != 0) {
                return agent::Error::none;
            }
            if ((bits & kDisconnected) != 0) {
                if (authentication_failure(
                        last_disconnect_reason_.load(
                            std::memory_order_acquire))) {
                    disconnect();
                    state_.store(
                        NetworkState::failed, std::memory_order_release);
                    return agent::Error::authentication;
                }
                break;
            }
        }
        disconnect();
    }
    if (connected()) {
        return agent::Error::none;
    }
    const agent::Error credentials_error = apply_credentials(credentials);
    if (credentials_error != agent::Error::none) {
        state_.store(NetworkState::failed, std::memory_order_release);
        return credentials_error;
    }
    if (connected()) {
        return agent::Error::none;
    }
    for (std::uint8_t attempt = 0; attempt < policy.max_attempts; ++attempt) {
        if (elapsed(
                total_start, monotonic_ms(), policy.total_timeout_ms)) {
            break;
        }
        if (cancellation.cancelled()) {
            disconnect(false);
            return agent::Error::cancelled;
        }
        if (connected()) {
            return agent::Error::none;
        }
        xEventGroupClearBits(events_, kConnected | kDisconnected);
        last_disconnect_reason_.store(0, std::memory_order_release);
        connect_started_at_ms_.store(monotonic_ms(), std::memory_order_release);
        connect_started_.store(true, std::memory_order_release);
        state_.store(NetworkState::connecting, std::memory_order_release);
        if (esp_wifi_connect() != ESP_OK) {
            connect_started_.store(false, std::memory_order_release);
            disconnect();
            state_.store(NetworkState::failed, std::memory_order_release);
            return agent::Error::disconnected;
        }
        const std::uint32_t attempt_start = monotonic_ms();
        while (!elapsed(
                   attempt_start, monotonic_ms(), policy.attempt_timeout_ms) &&
               !elapsed(total_start, monotonic_ms(), policy.total_timeout_ms)) {
            if (cancellation.cancelled()) {
                disconnect(false);
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
                state_.store(NetworkState::failed, std::memory_order_release);
                return agent::Error::authentication;
            }
            if ((bits & kDisconnected) != 0) {
                xEventGroupClearBits(events_, kDisconnected);
                break;
            }
        }
        disconnect();
        if (attempt + 1 < policy.max_attempts &&
            !elapsed(total_start, monotonic_ms(), policy.total_timeout_ms)) {
            std::uint32_t waited = 0;
            while (waited < policy.retry_delay_ms) {
                if (cancellation.cancelled()) {
                    disconnect(false);
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
    state_.store(NetworkState::failed, std::memory_order_release);
    return agent::Error::connect_timeout;
}

esp_err_t NetworkManager::set_request_active(bool active) {
    if (!initialized_ || !wifi_started_) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t error = esp_wifi_set_ps(
        active ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
    if (error == ESP_OK) {
        ESP_LOGI(
            kTag,
            "Wi-Fi request power mode: %s",
            active ? "active" : "modem-save");
    }
    return error;
}

void NetworkManager::disconnect() {
    disconnect(true);
}

void NetworkManager::disconnect(bool wait_for_event) {
    if (!initialized_) {
        return;
    }
    const bool active = connected() || connecting();
    xEventGroupClearBits(events_, kDisconnected);
    const esp_err_t result = esp_wifi_disconnect();
    connect_started_.store(false, std::memory_order_release);
    connect_started_at_ms_.store(0, std::memory_order_release);
    if (wait_for_event && active && result == ESP_OK) {
        xEventGroupWaitBits(
            events_, kDisconnected, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(kDisconnectDrainMs));
    }
    xEventGroupClearBits(events_, kConnected | kDisconnected);
    state_.store(NetworkState::off, std::memory_order_release);
}

void NetworkManager::shutdown() {
    if (!wifi_started_) {
        return;
    }
    disconnect(false);
    esp_wifi_stop();
    wifi_started_ = false;
    state_.store(NetworkState::off, std::memory_order_release);
}

NetworkState NetworkManager::state() const {
    return state_.load(std::memory_order_acquire);
}

bool NetworkManager::connected() const {
    return initialized_ && state() == NetworkState::connected;
}

bool NetworkManager::connecting() const {
    return initialized_ && state() == NetworkState::connecting;
}

std::uint8_t NetworkManager::rssi_band() const {
    if (!connected()) {
        return 0;
    }
    wifi_ap_record_t access_point{};
    if (esp_wifi_sta_get_ap_info(&access_point) != ESP_OK) {
        return 0;
    }
    if (access_point.rssi >= -65) {
        return 1;
    }
    return access_point.rssi >= -75 ? 2 : 3;
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
        connect_started_.store(false, std::memory_order_release);
        connect_started_at_ms_.store(0, std::memory_order_release);
        state_.store(NetworkState::connected, std::memory_order_release);
        xEventGroupSetBits(events_, kConnected);
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        connect_started_.store(false, std::memory_order_release);
        connect_started_at_ms_.store(0, std::memory_order_release);
        if (event_data != nullptr) {
            last_disconnect_reason_.store(
                static_cast<wifi_event_sta_disconnected_t *>(event_data)->reason,
                std::memory_order_release);
        }
        state_.store(NetworkState::failed, std::memory_order_release);
        xEventGroupClearBits(events_, kConnected);
        xEventGroupSetBits(events_, kDisconnected);
    }
}

void NetworkManager::clean_failed_initialize() {
    connect_started_.store(false, std::memory_order_release);
    connect_started_at_ms_.store(0, std::memory_order_release);
    state_.store(NetworkState::off, std::memory_order_release);
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
