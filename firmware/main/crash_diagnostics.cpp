#include "crash_diagnostics.hpp"

#include <cstdint>
#include <type_traits>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

namespace chatesp {
namespace crash_diagnostics {
namespace {

constexpr char kTag[] = "crash_trace";
RTC_NOINIT_ATTR runtime::CrashTraceStore s_store;
static_assert(
    std::is_trivial_v<runtime::CrashTraceStore>,
    "The RTC trace must not run a startup constructor");
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

std::uint32_t monotonic_ms() {
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1'000ULL);
}

const char *event_name(runtime::CrashEvent event) {
    using Event = runtime::CrashEvent;
    switch (event) {
        case Event::boot_enter: return "boot_enter";
        case Event::power_ready: return "power_ready";
        case Event::display_ready: return "display_ready";
        case Event::runtime_ready: return "runtime_ready";
        case Event::wifi_connected: return "wifi_connected";
        case Event::ble_start_begin: return "ble_start_begin";
        case Event::ble_start_complete: return "ble_start_complete";
        case Event::ble_stop_requested: return "ble_stop_requested";
        case Event::ble_stop_task_start: return "ble_stop_task_start";
        case Event::ble_host_stop_begin: return "ble_host_stop_begin";
        case Event::ble_host_stop_complete: return "ble_host_stop_complete";
        case Event::ble_store_capture_begin: return "ble_store_capture_begin";
        case Event::ble_store_capture_complete:
            return "ble_store_capture_complete";
        case Event::ble_deinit_begin: return "ble_deinit_begin";
        case Event::ble_deinit_complete: return "ble_deinit_complete";
        case Event::ble_stop_complete: return "ble_stop_complete";
        case Event::network_context_begin: return "network_context_begin";
        case Event::network_context_complete:
            return "network_context_complete";
        case Event::ble_passkey_begin: return "ble_passkey_begin";
        case Event::ble_passkey_complete: return "ble_passkey_complete";
        case Event::ble_connected: return "ble_connected";
        case Event::ble_secure: return "ble_secure";
        case Event::ble_disconnected: return "ble_disconnected";
        case Event::settings_transfer_begin: return "settings_transfer_begin";
        case Event::settings_packet_complete:
            return "settings_packet_complete";
        case Event::settings_ack_waiting: return "settings_ack_waiting";
        case Event::settings_ack_sent: return "settings_ack_sent";
        case Event::settings_ack_confirmed: return "settings_ack_confirmed";
        case Event::settings_ack_failed: return "settings_ack_failed";
        case Event::settings_apply_begin: return "settings_apply_begin";
        case Event::settings_apply_complete: return "settings_apply_complete";
        case Event::soft_sleep_begin: return "soft_sleep_begin";
        case Event::poweroff_begin: return "poweroff_begin";
    }
    return "unknown";
}

void log_boot(const runtime::CrashBootRecord &boot) {
    if (!runtime::crash_boot_record_valid(boot) || boot.active != 0) {
        return;
    }
    ESP_LOGW(
        kTag,
        "Previous boot %u ended with reset reason %u after %u events",
        static_cast<unsigned>(boot.sequence),
        static_cast<unsigned>(boot.outcome_reset_reason),
        static_cast<unsigned>(boot.event_count));
    ESP_LOGW(
        kTag, "Previous boot %u runtime heartbeat: %u ms",
        static_cast<unsigned>(boot.sequence),
        static_cast<unsigned>(boot.last_heartbeat_ms));
    const std::size_t first =
        (boot.next_event + boot.events.size() - boot.event_count) %
        boot.events.size();
    for (std::size_t offset = 0; offset < boot.event_count; ++offset) {
        const runtime::CrashEventRecord &event =
            boot.events[(first + offset) % boot.events.size()];
        ESP_LOGW(
            kTag, "Previous boot %u: %u ms %s",
            static_cast<unsigned>(boot.sequence),
            static_cast<unsigned>(event.at_ms), event_name(event.event));
    }
}

}  // namespace

void initialize() {
    portENTER_CRITICAL(&s_lock);
    runtime::crash_trace_begin_boot(
        s_store, static_cast<std::uint32_t>(esp_reset_reason()));
    portEXIT_CRITICAL(&s_lock);
    for (const runtime::CrashBootRecord &boot : s_store.boots) {
        log_boot(boot);
    }
    mark(runtime::CrashEvent::boot_enter);
}

void mark(runtime::CrashEvent event) {
    const std::uint32_t now_ms = monotonic_ms();
    portENTER_CRITICAL(&s_lock);
    (void)runtime::crash_trace_mark(s_store, event, now_ms);
    portEXIT_CRITICAL(&s_lock);
}

void heartbeat() {
    const std::uint32_t now_ms = monotonic_ms();
    portENTER_CRITICAL(&s_lock);
    (void)runtime::crash_trace_heartbeat(s_store, now_ms);
    portEXIT_CRITICAL(&s_lock);
}

}  // namespace crash_diagnostics
}  // namespace chatesp
