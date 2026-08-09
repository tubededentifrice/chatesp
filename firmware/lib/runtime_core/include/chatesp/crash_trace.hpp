#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace chatesp {
namespace runtime {

enum class CrashEvent : std::uint16_t {
    boot_enter = 1,
    power_ready,
    display_ready,
    runtime_ready,
    wifi_connected,
    ble_start_begin,
    ble_start_complete,
    ble_memory_recovery_restart,
    ble_stop_requested,
    ble_stop_task_start,
    ble_disconnect_requested,
    ble_disconnect_complete,
    ble_disconnect_timeout,
    ble_host_stop_begin,
    ble_host_stop_complete,
    ble_store_capture_begin,
    ble_store_capture_complete,
    ble_deinit_begin,
    ble_deinit_complete,
    ble_stop_complete,
    network_context_begin,
    network_context_complete,
    ble_passkey_begin,
    ble_passkey_complete,
    ble_connected,
    ble_secure,
    ble_security_start_failed,
    ble_security_failed,
    ble_repeat_pairing,
    ble_disconnected,
    settings_transfer_begin,
    settings_packet_complete,
    settings_ack_waiting,
    settings_ack_sent,
    settings_ack_confirmed,
    settings_ack_failed,
    settings_apply_begin,
    settings_apply_complete,
    memory_command_begin,
    memory_response_queued,
    memory_indication_sent,
    pwr_raw_press,
    pwr_raw_release,
    pwr_raw_short_press,
    pwr_raw_long_press,
    pwr_power_source_change,
    pwr_press_accepted,
    pwr_release_accepted,
    pwr_release_unconfirmed,
    sleep_button_request,
    sleep_model_request,
    soft_sleep_begin,
    poweroff_begin,
    display_sleep_complete,
    display_wake_begin,
    display_wake_complete,
    display_wake_failed,
    pwr_poll_failed,
    pwr_poll_recovered,
    pwr_legacy_policy_repaired,
    ble_connection_failed,
    ble_advertise_failed,
    ble_advertise_restarted,
    ble_advertise_retry,
    ble_advertise_recovery,
    phone_proxy_wake_start,
    phone_proxy_grace_begin,
    phone_proxy_ready,
    phone_proxy_fallback,
};

constexpr std::size_t kCrashTraceBootCount = 3;
constexpr std::size_t kCrashTraceEventCount = 32;

struct CrashEventRecord {
    std::uint32_t at_ms;
    CrashEvent event;
    std::uint16_t reserved;
};

struct CrashBootRecord {
    std::uint32_t sequence;
    std::uint32_t start_reset_reason;
    std::uint32_t outcome_reset_reason;
    std::uint8_t active;
    std::uint8_t event_count;
    std::uint8_t next_event;
    std::uint8_t reserved;
    std::array<CrashEventRecord, kCrashTraceEventCount> events;
    std::uint32_t last_heartbeat_ms;
    std::uint32_t checksum;
};

struct CrashTraceStore {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t reserved;
    std::array<CrashBootRecord, kCrashTraceBootCount> boots;
};

bool crash_trace_valid(const CrashTraceStore &store);
bool crash_boot_record_valid(const CrashBootRecord &boot);
std::size_t crash_trace_active_index(const CrashTraceStore &store);
void crash_trace_begin_boot(
    CrashTraceStore &store, std::uint32_t reset_reason);
bool crash_trace_mark(
    CrashTraceStore &store, CrashEvent event, std::uint32_t at_ms);
bool crash_trace_heartbeat(CrashTraceStore &store, std::uint32_t at_ms);

}  // namespace runtime
}  // namespace chatesp
