#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>

#include "chatesp/agent_interfaces.hpp"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

namespace chatesp {
namespace network {

struct WifiCredentials {
    const char *ssid = nullptr;
    std::size_t ssid_size = 0;
    const char *password = nullptr;
    std::size_t password_size = 0;
};

struct ConnectPolicy {
    std::uint32_t attempt_timeout_ms = 8'000;
    std::uint32_t total_timeout_ms = 25'000;
    std::uint32_t retry_delay_ms = 300;
    std::uint8_t max_attempts = 3;
};

enum class NetworkState : std::uint8_t {
    off,
    connecting,
    connected,
    failed,
};

class NetworkManager {
public:
    esp_err_t initialize();
    agent::Error start_connect(const WifiCredentials &credentials);
    agent::Error connect(
        const WifiCredentials &credentials,
        agent::CancellationToken &cancellation,
        const ConnectPolicy &policy = {});
    void disconnect();
    void shutdown();
    [[nodiscard]] NetworkState state() const;
    esp_err_t set_request_active(bool active);
    [[nodiscard]] bool connected() const;
    [[nodiscard]] bool connecting() const;

private:
    static void event_handler(
        void *argument, esp_event_base_t event_base, std::int32_t event_id,
        void *event_data);
    void handle_event(
        esp_event_base_t event_base, std::int32_t event_id, void *event_data);
    void clean_failed_initialize();
    void disconnect(bool wait_for_event);
    agent::Error apply_credentials(const WifiCredentials &credentials);
    agent::Error ensure_wifi_started();

    EventGroupHandle_t events_ = nullptr;
    esp_netif_t *station_ = nullptr;
    esp_event_handler_instance_t wifi_events_ = nullptr;
    esp_event_handler_instance_t ip_events_ = nullptr;
    std::atomic<std::uint8_t> last_disconnect_reason_{0};
    std::atomic<bool> connect_started_{false};
    std::atomic<std::uint32_t> connect_started_at_ms_{0};
    std::atomic<NetworkState> state_{NetworkState::off};
    bool initialized_ = false;
    bool wifi_driver_initialized_ = false;
    bool wifi_started_ = false;
};

}  // namespace network
}  // namespace chatesp
