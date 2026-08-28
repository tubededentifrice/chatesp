#include "ble_provisioning.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "chatesp/ble_shutdown.hpp"
#include "chatesp/indication_gate.hpp"
#include "chatesp/provisioning_session.hpp"
#include "chatesp/runtime_control.hpp"
#include "crash_diagnostics.hpp"
#include "device_memory_store.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "host/ble_gap.h"
#include "host/ble_att.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/nimble_npl.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "settings_store.hpp"
#include "resource_telemetry.hpp"
#include "task_config.hpp"

extern "C" void ble_store_config_init(void);

#if CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE < 8192
#error "The NimBLE host stack must support bounded memory BLE commands"
#endif

namespace chatesp {
namespace ble_provisioning {
namespace {

constexpr char kDeviceName[] = "ChatESP";
constexpr std::uint16_t kNoConnection = BLE_HS_CONN_HANDLE_NONE;
constexpr std::size_t kMaximumDataFrameSize =
    provisioning::kDataFrameHeaderSize + provisioning::kMaximumFrameDataSize;
constexpr std::uint32_t kStopTaskReclaimDelayMs = 10;
constexpr std::uint32_t kLifecyclePollMs = 1;
constexpr std::uint32_t kDisconnectTimeoutMs = 2'000;
constexpr std::uint32_t kAdvertiseRetryMs = 100;
constexpr std::uint8_t kAdvertiseRetryLimit = 5;
constexpr std::uint32_t kAcknowledgementRetryMs = 100;
constexpr char kLogTag[] = "ble_provisioning";

#if !defined(CONFIG_BT_NIMBLE_NVS_PERSIST) || \
    !CONFIG_BT_NIMBLE_NVS_PERSIST
constexpr std::size_t kVolatileStoreCapacity =
    3U * CONFIG_BT_NIMBLE_MAX_BONDS + CONFIG_BT_NIMBLE_MAX_CCCDS;

struct VolatileStoreEntry {
    int object_type = 0;
    union ble_store_value value{};
};

struct VolatileStoreCapture {
    std::array<VolatileStoreEntry, kVolatileStoreCapacity> entries{};
    std::size_t count = 0;
    bool overflow = false;
};

VolatileStoreCapture s_volatile_store;
bool s_volatile_store_valid = false;

int capture_volatile_store_entry(
    int object_type,
    union ble_store_value *value,
    void *cookie) {
    auto *capture = static_cast<VolatileStoreCapture *>(cookie);
    if (capture == nullptr || value == nullptr ||
        capture->count >= capture->entries.size()) {
        if (capture != nullptr) {
            capture->overflow = true;
        }
        return 1;
    }
    capture->entries[capture->count++] = {object_type, *value};
    return 0;
}

bool capture_volatile_store() {
    // The BLE stop task has a small internal-RAM stack. Capture directly into
    // bounded static storage so a complete bond store cannot overflow it.
    s_volatile_store.count = 0;
    s_volatile_store.overflow = false;
    s_volatile_store_valid = false;
    constexpr std::array<int, 5> kObjectTypes{
        BLE_STORE_OBJ_TYPE_OUR_SEC,
        BLE_STORE_OBJ_TYPE_PEER_SEC,
        BLE_STORE_OBJ_TYPE_CCCD,
        BLE_STORE_OBJ_TYPE_PEER_ADDR,
        BLE_STORE_OBJ_TYPE_LOCAL_IRK,
    };
    for (const int object_type : kObjectTypes) {
        if (ble_store_iterate(
                object_type, capture_volatile_store_entry,
                &s_volatile_store) != 0 ||
            s_volatile_store.overflow) {
            s_volatile_store.count = 0;
            s_volatile_store.overflow = false;
            return false;
        }
    }
    s_volatile_store_valid = true;
    return true;
}

bool restore_volatile_store() {
    if (!s_volatile_store_valid) {
        return true;
    }
    for (std::size_t index = 0; index < s_volatile_store.count; ++index) {
        const auto &entry = s_volatile_store.entries[index];
        if (ble_store_write(entry.object_type, &entry.value) != 0) {
            return false;
        }
    }
    return true;
}
#else
bool capture_volatile_store() { return true; }
bool restore_volatile_store() { return true; }
#endif

const ble_uuid128_t kServiceUuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x50, 0x53, 0x45, 0x4c, 0x71, 0x9d,
    0x8a, 0x4b, 0x3c, 0x6f, 0x00, 0x10, 0x2e, 0x7b);
const ble_uuid128_t kControlUuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x50, 0x53, 0x45, 0x4c, 0x71, 0x9d,
    0x8a, 0x4b, 0x3c, 0x6f, 0x01, 0x10, 0x2e, 0x7b);
const ble_uuid128_t kDataUuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x50, 0x53, 0x45, 0x4c, 0x71, 0x9d,
    0x8a, 0x4b, 0x3c, 0x6f, 0x02, 0x10, 0x2e, 0x7b);
const ble_uuid128_t kAcknowledgementUuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x50, 0x53, 0x45, 0x4c, 0x71, 0x9d,
    0x8a, 0x4b, 0x3c, 0x6f, 0x03, 0x10, 0x2e, 0x7b);
const ble_uuid128_t kDeviceContextUuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x50, 0x53, 0x45, 0x4c, 0x71, 0x9d,
    0x8a, 0x4b, 0x3c, 0x6f, 0x04, 0x10, 0x2e, 0x7b);
const ble_uuid128_t kMemoryCommandUuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x50, 0x53, 0x45, 0x4c, 0x71, 0x9d,
    0x8a, 0x4b, 0x3c, 0x6f, 0x05, 0x10, 0x2e, 0x7b);
const ble_uuid128_t kMemoryResponseUuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x50, 0x53, 0x45, 0x4c, 0x71, 0x9d,
    0x8a, 0x4b, 0x3c, 0x6f, 0x06, 0x10, 0x2e, 0x7b);
const ble_uuid128_t kHttpProxyServiceUuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x50, 0x53, 0x45, 0x4c, 0x71, 0x9d,
    0x8a, 0x4b, 0x3c, 0x6f, 0x00, 0x21, 0x2e, 0x7b);
const ble_uuid128_t kHttpProxyRequestUuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x50, 0x53, 0x45, 0x4c, 0x71, 0x9d,
    0x8a, 0x4b, 0x3c, 0x6f, 0x01, 0x21, 0x2e, 0x7b);
const ble_uuid128_t kHttpProxyResponseUuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x50, 0x53, 0x45, 0x4c, 0x71, 0x9d,
    0x8a, 0x4b, 0x3c, 0x6f, 0x02, 0x21, 0x2e, 0x7b);

SettingsStore *s_settings_store = nullptr;
DeviceMemoryStore *s_memory_store = nullptr;
PasskeyCallback s_passkey_callback = nullptr;
DeviceContextCallback s_device_context_callback = nullptr;
void *s_callback_context = nullptr;
provisioning::ProvisioningSession s_session;
std::uint16_t s_owner_connection = kNoConnection;
std::uint16_t s_passkey_connection = kNoConnection;
std::uint16_t s_acknowledgement_handle = 0;
std::uint16_t s_memory_response_handle = 0;
std::uint16_t s_http_proxy_request_handle = 0;
std::uint16_t s_active_connection = kNoConnection;
SemaphoreHandle_t s_memory_ble_mutex = nullptr;
std::array<std::uint8_t, agent::max_memory_ble_response_bytes>
    s_queued_memory_response{};
std::size_t s_queued_memory_response_size = 0;
std::uint16_t s_queued_memory_connection = kNoConnection;
agent::MemorySnapshot s_pending_memory_change;
bool s_memory_change_pending = false;
std::atomic<bool> s_memory_indication_in_flight{false};
bool s_memory_command_active = false;
std::array<std::uint8_t, agent::max_memory_ble_command_bytes>
    s_cached_memory_command{};
std::size_t s_cached_memory_command_size = 0;
std::array<std::uint8_t, agent::max_memory_ble_response_bytes>
    s_cached_memory_response{};
std::size_t s_cached_memory_response_size = 0;
std::uint16_t s_cached_memory_connection = kNoConnection;
std::uint8_t s_address_type = 0;
std::atomic<bool> s_running{false};
enum class LifecycleState : std::uint8_t {
    idle,
    starting,
    stopping,
};
std::atomic<LifecycleState> s_lifecycle{LifecycleState::idle};
std::atomic<bool> s_bond_available{false};
ble_npl_callout s_advertise_retry_callout{};
bool s_advertise_retry_initialized = false;
std::uint8_t s_advertise_retry_count = 0;
std::atomic<bool> s_advertise_recovery_requested{false};
ble_npl_callout s_acknowledgement_retry_callout{};
bool s_acknowledgement_retry_initialized = false;
runtime::BleShutdown s_shutdown;
runtime::AsyncShutdownGate s_stop_gate;
SemaphoreHandle_t s_stop_done = nullptr;
SemaphoreHandle_t s_disconnect_done = nullptr;
std::atomic<std::uint16_t> s_disconnect_wait_connection{kNoConnection};
std::atomic<esp_err_t> s_stop_result{ESP_OK};
provisioning::IndicationGate s_acknowledgement_gate;
std::atomic<bool> s_acknowledgement_waiting{false};
std::atomic<bool> s_acknowledgement_in_flight{false};
std::atomic<bool> s_settings_confirmation_pending{false};
std::atomic<bool> s_indication_send_active{false};
std::atomic<bool> s_http_proxy_subscribed{false};
std::atomic<bool> s_http_proxy_secure{false};
SemaphoreHandle_t s_http_proxy_send_mutex = nullptr;
QueueHandle_t s_http_proxy_response_queue = nullptr;

struct HttpProxyFrame {
    std::uint16_t size = 0;
    std::array<std::uint8_t, kMaximumHttpProxyFrameSize> data{};
};

class StartGateGuard {
public:
    explicit StartGateGuard(bool active) : active_(active) {}
    ~StartGateGuard() {
        if (active_) {
            s_lifecycle.store(
                LifecycleState::idle, std::memory_order_release);
        }
    }

    StartGateGuard(const StartGateGuard &) = delete;
    StartGateGuard &operator=(const StartGateGuard &) = delete;

private:
    bool active_ = false;
};

void delete_http_proxy_resources() {
    if (s_http_proxy_send_mutex != nullptr) {
        vSemaphoreDelete(s_http_proxy_send_mutex);
        s_http_proxy_send_mutex = nullptr;
    }
    if (s_http_proxy_response_queue != nullptr) {
        vQueueDelete(s_http_proxy_response_queue);
        s_http_proxy_response_queue = nullptr;
    }
}

int gap_event(ble_gap_event *event, void *argument);
int gatt_access(
    std::uint16_t connection_handle,
    std::uint16_t attribute_handle,
    ble_gatt_access_ctxt *context,
    void *argument);

struct BondInventory {
    std::size_t local = 0;
    std::size_t peer = 0;
    ble_addr_t local_address{};
    ble_addr_t peer_address{};
    bool local_ltk = false;
    bool local_identity = false;
    bool peer_ltk = false;
    bool peer_identity = false;
};

int inspect_bond_entry(
    int object_type, union ble_store_value *value, void *cookie) {
    auto *inventory = static_cast<BondInventory *>(cookie);
    if (inventory == nullptr || value == nullptr) {
        return 1;
    }
    if (object_type == BLE_STORE_OBJ_TYPE_OUR_SEC) {
        ++inventory->local;
        inventory->local_address = value->sec.peer_addr;
        inventory->local_ltk = value->sec.ltk_present;
        inventory->local_identity = value->sec.irk_present;
    } else if (object_type == BLE_STORE_OBJ_TYPE_PEER_SEC) {
        ++inventory->peer;
        inventory->peer_address = value->sec.peer_addr;
        inventory->peer_ltk = value->sec.ltk_present;
        inventory->peer_identity = value->sec.irk_present;
    }
    return 0;
}

BondInventory inspect_bond_store() {
    BondInventory inventory;
    if (ble_store_iterate(
            BLE_STORE_OBJ_TYPE_OUR_SEC, inspect_bond_entry, &inventory) != 0 ||
        ble_store_iterate(
            BLE_STORE_OBJ_TYPE_PEER_SEC, inspect_bond_entry, &inventory) != 0) {
        return {};
    }
    return inventory;
}

bool addresses_match(const ble_addr_t &left, const ble_addr_t &right) {
    return left.type == right.type &&
        std::memcmp(left.val, right.val, sizeof(left.val)) == 0;
}

bool complete_bond_is_stored(const BondInventory &inventory) {
    return inventory.local == 1 && inventory.peer == 1 &&
        inventory.local_ltk && inventory.local_identity &&
        inventory.peer_ltk && inventory.peer_identity &&
        addresses_match(inventory.local_address, inventory.peer_address);
}

bool remove_stored_bond(const BondInventory &inventory) {
    if (inventory.local == 0 && inventory.peer == 0) {
        return true;
    }
    const ble_addr_t &address = inventory.peer != 0
        ? inventory.peer_address
        : inventory.local_address;
    const int result = ble_store_util_delete_peer(&address);
    if (result != 0) {
        ESP_LOGW(
            kLogTag, "Could not remove stale BLE bond (category %d)", result);
        return false;
    }
    s_bond_available.store(false, std::memory_order_release);
    ESP_LOGW(kLogTag, "Removed one stale BLE bond");
    return true;
}

ble_gatt_chr_def s_characteristics[] = {
    {
        .uuid = &kControlUuid.u,
        .access_cb = gatt_access,
        .arg = reinterpret_cast<void *>(1),
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
            BLE_GATT_CHR_F_WRITE_AUTHEN,
        .min_key_size = BLE_SM_PAIR_KEY_SZ_MAX,
        .val_handle = nullptr,
        .cpfd = nullptr,
    },
    {
        .uuid = &kDataUuid.u,
        .access_cb = gatt_access,
        .arg = reinterpret_cast<void *>(2),
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
            BLE_GATT_CHR_F_WRITE_AUTHEN,
        .min_key_size = BLE_SM_PAIR_KEY_SZ_MAX,
        .val_handle = nullptr,
        .cpfd = nullptr,
    },
    {
        .uuid = &kAcknowledgementUuid.u,
        .access_cb = gatt_access,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_INDICATE |
            BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC |
            BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN,
        .min_key_size = BLE_SM_PAIR_KEY_SZ_MAX,
        .val_handle = &s_acknowledgement_handle,
        .cpfd = nullptr,
    },
    {
        .uuid = &kDeviceContextUuid.u,
        .access_cb = gatt_access,
        .arg = reinterpret_cast<void *>(3),
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
            BLE_GATT_CHR_F_WRITE_AUTHEN,
        .min_key_size = BLE_SM_PAIR_KEY_SZ_MAX,
        .val_handle = nullptr,
        .cpfd = nullptr,
    },
    {
        .uuid = &kMemoryCommandUuid.u,
        .access_cb = gatt_access,
        .arg = reinterpret_cast<void *>(4),
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
            BLE_GATT_CHR_F_WRITE_AUTHEN,
        .min_key_size = BLE_SM_PAIR_KEY_SZ_MAX,
        .val_handle = nullptr,
        .cpfd = nullptr,
    },
    {
        .uuid = &kMemoryResponseUuid.u,
        .access_cb = gatt_access,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_INDICATE |
            BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC |
            BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN,
        .min_key_size = BLE_SM_PAIR_KEY_SZ_MAX,
        .val_handle = &s_memory_response_handle,
        .cpfd = nullptr,
    },
    {},
};

ble_gatt_chr_def s_http_proxy_characteristics[] = {
    {
        .uuid = &kHttpProxyRequestUuid.u,
        .access_cb = gatt_access,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_NOTIFY |
            BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC |
            BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN,
        .min_key_size = BLE_SM_PAIR_KEY_SZ_MAX,
        .val_handle = &s_http_proxy_request_handle,
        .cpfd = nullptr,
    },
    {
        .uuid = &kHttpProxyResponseUuid.u,
        .access_cb = gatt_access,
        .arg = reinterpret_cast<void *>(5),
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
            BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN,
        .min_key_size = BLE_SM_PAIR_KEY_SZ_MAX,
        .val_handle = nullptr,
        .cpfd = nullptr,
    },
    {},
};

ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kServiceUuid.u,
        .includes = nullptr,
        .characteristics = s_characteristics,
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kHttpProxyServiceUuid.u,
        .includes = nullptr,
        .characteristics = s_http_proxy_characteristics,
    },
    {},
};

void show_passkey(std::uint32_t passkey, bool visible) {
    if (s_passkey_callback != nullptr) {
        s_passkey_callback(passkey, visible, s_callback_context);
    }
}

void clear_bytes(void *data, std::size_t size) {
    volatile std::uint8_t *cursor = static_cast<volatile std::uint8_t *>(data);
    for (std::size_t index = 0; index < size; ++index) {
        cursor[index] = 0;
    }
}

void hide_passkey(std::uint16_t connection_handle) {
    if (s_passkey_connection == connection_handle) {
        show_passkey(0, false);
        s_passkey_connection = kNoConnection;
    }
}

void hide_all_passkeys() {
    if (s_passkey_connection != kNoConnection) {
        show_passkey(0, false);
        s_passkey_connection = kNoConnection;
    }
}

provisioning::LinkSecurity link_security(std::uint16_t connection_handle) {
    ble_gap_conn_desc description{};
    if (ble_gap_conn_find(connection_handle, &description) != 0) {
        return {};
    }
    return {
        description.sec_state.encrypted != 0,
        description.sec_state.authenticated != 0,
        description.sec_state.bonded != 0,
        description.sec_state.key_size == BLE_SM_PAIR_KEY_SZ_MAX &&
            ble_hs_cfg.sm_sc_only != 0,
    };
}

void try_send_acknowledgement();

void sync_acknowledgement_state() {
    s_acknowledgement_waiting.store(
        s_acknowledgement_gate.waiting(), std::memory_order_release);
    s_acknowledgement_in_flight.store(
        s_acknowledgement_gate.in_flight(), std::memory_order_release);
    s_settings_confirmation_pending.store(
        s_acknowledgement_gate.settings_confirmation_pending(),
        std::memory_order_release);
}

bool schedule_acknowledgement_retry() {
    if (!s_running.load(std::memory_order_acquire) ||
        s_stop_gate.running() || !s_acknowledgement_retry_initialized) {
        return false;
    }
    return ble_npl_callout_reset(
               &s_acknowledgement_retry_callout,
               ble_npl_time_ms_to_ticks32(kAcknowledgementRetryMs)) ==
        BLE_NPL_OK;
}

void handle_acknowledgement_failure(
    provisioning::IndicationDeliveryAction action) {
    sync_acknowledgement_state();
    const bool retry =
        action == provisioning::IndicationDeliveryAction::retry ||
        action == provisioning::IndicationDeliveryAction::settings_retry;
    if (action == provisioning::IndicationDeliveryAction::settings_retry ||
        action == provisioning::IndicationDeliveryAction::settings_failed) {
        crash_diagnostics::mark(runtime::CrashEvent::settings_ack_failed);
    }
    if (retry) {
        if (!schedule_acknowledgement_retry() &&
            s_running.load(std::memory_order_acquire) &&
            !s_stop_gate.running()) {
            s_acknowledgement_gate.reset_delivery();
            sync_acknowledgement_state();
            ESP_LOGW(kLogTag, "BLE acknowledgement retry timer failed");
        }
        return;
    }
    try_send_acknowledgement();
}

void send_acknowledgement(
    std::uint16_t connection_handle,
    const provisioning::SessionResult &result) {
    if (!result.has_acknowledgement || s_acknowledgement_handle == 0) {
        return;
    }
    const bool settings_confirmation =
        result.acknowledgement[5] == static_cast<std::uint8_t>(
            provisioning::AcknowledgementStatus::applied) ||
        result.acknowledgement[5] == static_cast<std::uint8_t>(
            provisioning::AcknowledgementStatus::unchanged);
    if (!s_acknowledgement_gate.enqueue(
            connection_handle, provisioning::IndicationKind::settings,
            result.acknowledgement.data(), result.acknowledgement.size(),
            settings_confirmation)) {
        if (settings_confirmation) {
            crash_diagnostics::mark(
                runtime::CrashEvent::settings_ack_failed);
        }
        return;
    }
    sync_acknowledgement_state();
    if (settings_confirmation) {
        crash_diagnostics::mark(runtime::CrashEvent::settings_ack_waiting);
    }
    try_send_acknowledgement();
}

void send_device_context_acknowledgement(
    std::uint16_t connection_handle,
    const std::array<std::uint8_t,
        provisioning::kDeviceContextAcknowledgementSize> &acknowledgement) {
    if (s_acknowledgement_handle == 0) {
        return;
    }
    if (s_acknowledgement_gate.enqueue(
            connection_handle, provisioning::IndicationKind::device_context,
            acknowledgement.data(), acknowledgement.size(), false)) {
        sync_acknowledgement_state();
    }
    try_send_acknowledgement();
}

void try_send_acknowledgement() {
    if (s_indication_send_active.load(std::memory_order_acquire) ||
        s_memory_indication_in_flight.load(std::memory_order_acquire) ||
        s_acknowledgement_in_flight.load(std::memory_order_acquire) ||
        s_acknowledgement_handle == 0) {
        return;
    }
    std::uint16_t connection_handle = kNoConnection;
    const std::uint8_t *data = nullptr;
    std::size_t size = 0;
    if (!s_acknowledgement_gate.pending(
            connection_handle, data, size)) {
        return;
    }
    if (!s_acknowledgement_gate.begin_attempt()) {
        return;
    }
    os_mbuf *packet = ble_hs_mbuf_from_flat(data, size);
    if (packet == nullptr) {
        handle_acknowledgement_failure(
            s_acknowledgement_gate.send_failed());
        return;
    }
    s_indication_send_active.store(true, std::memory_order_release);
    const int result = ble_gatts_indicate_custom(
        connection_handle, s_acknowledgement_handle, packet);
    s_indication_send_active.store(false, std::memory_order_release);
    if (result != 0) {
        handle_acknowledgement_failure(
            s_acknowledgement_gate.send_failed());
        return;
    }
    (void)s_acknowledgement_gate.mark_sent();
    sync_acknowledgement_state();
    if (s_settings_confirmation_pending.load(std::memory_order_acquire)) {
        crash_diagnostics::mark(runtime::CrashEvent::settings_ack_sent);
    }
}

bool memory_ble_lock() {
    return s_memory_ble_mutex != nullptr &&
        xSemaphoreTake(s_memory_ble_mutex, pdMS_TO_TICKS(1'000)) == pdTRUE;
}

void memory_ble_unlock() {
    if (s_memory_ble_mutex != nullptr) {
        xSemaphoreGive(s_memory_ble_mutex);
    }
}

agent::MemoryBleStatus memory_ble_status(
    agent::MemoryMutationStatus status) {
    switch (status) {
        case agent::MemoryMutationStatus::applied:
            return agent::MemoryBleStatus::applied;
        case agent::MemoryMutationStatus::unchanged:
            return agent::MemoryBleStatus::unchanged;
        case agent::MemoryMutationStatus::full:
            return agent::MemoryBleStatus::full;
        case agent::MemoryMutationStatus::not_found:
            return agent::MemoryBleStatus::not_found;
        case agent::MemoryMutationStatus::revision_conflict:
            return agent::MemoryBleStatus::revision_conflict;
        case agent::MemoryMutationStatus::invalid_field:
            return agent::MemoryBleStatus::invalid_field;
        case agent::MemoryMutationStatus::storage_failure:
            return agent::MemoryBleStatus::storage_failure;
    }
    return agent::MemoryBleStatus::invalid_field;
}

void try_send_memory_indication_locked() {
    if (s_indication_send_active.load(std::memory_order_acquire) ||
        s_memory_indication_in_flight.load(std::memory_order_acquire) ||
        s_acknowledgement_waiting.load(std::memory_order_acquire) ||
        s_acknowledgement_in_flight.load(std::memory_order_acquire) ||
        s_memory_response_handle == 0 ||
        s_active_connection == kNoConnection) {
        return;
    }
    std::array<std::uint8_t, agent::max_memory_ble_response_bytes> encoded{};
    const std::uint8_t *data = nullptr;
    std::size_t size = 0;
    bool sending_change = false;
    if (s_queued_memory_response_size != 0 &&
        s_queued_memory_connection == s_active_connection) {
        data = s_queued_memory_response.data();
        size = s_queued_memory_response_size;
    } else if (s_memory_change_pending) {
        agent::MemoryBleResponse response;
        response.status = agent::MemoryBleStatus::applied;
        response.operation = agent::MemoryBleOperation::changed;
        response.revision = s_pending_memory_change.revision;
        response.fingerprint = s_pending_memory_change.fingerprint;
        response.total_count =
            static_cast<std::uint8_t>(s_pending_memory_change.size);
        response.changed_event = true;
        if (!agent::encode_memory_ble_response(response, encoded, size)) {
            s_memory_change_pending = false;
            return;
        }
        data = encoded.data();
        sending_change = true;
    } else {
        return;
    }
    os_mbuf *packet = ble_hs_mbuf_from_flat(data, size);
    if (packet == nullptr) {
        return;
    }
    s_indication_send_active.store(true, std::memory_order_release);
    const int result = ble_gatts_indicate_custom(
        s_active_connection, s_memory_response_handle, packet);
    s_indication_send_active.store(false, std::memory_order_release);
    if (result == 0) {
        s_memory_indication_in_flight.store(true, std::memory_order_release);
        crash_diagnostics::mark(
            runtime::CrashEvent::memory_indication_sent);
        if (sending_change) {
            s_memory_change_pending = false;
        } else {
            s_queued_memory_response.fill(0);
            s_queued_memory_response_size = 0;
            s_queued_memory_connection = kNoConnection;
        }
    }
}

void queue_memory_response_locked(
    std::uint16_t connection_handle,
    const std::array<std::uint8_t, agent::max_memory_ble_response_bytes> &encoded,
    std::size_t size) {
    if (size == 0 || size > s_queued_memory_response.size()) {
        return;
    }
    s_queued_memory_response = encoded;
    s_queued_memory_response_size = size;
    s_queued_memory_connection = connection_handle;
    crash_diagnostics::mark(runtime::CrashEvent::memory_response_queued);
    try_send_memory_indication_locked();
}

void memory_changed_callback(
    const agent::MemorySnapshot &snapshot, void *) {
    if (!memory_ble_lock()) {
        return;
    }
    s_pending_memory_change = snapshot;
    s_memory_change_pending = true;
    if (!s_memory_command_active) {
        try_send_memory_indication_locked();
    }
    memory_ble_unlock();
}

bool all_zero(const agent::MemoryFingerprint &fingerprint) {
    std::uint8_t value = 0;
    for (const std::uint8_t byte : fingerprint) {
        value |= byte;
    }
    return value == 0;
}

std::uint32_t read_memory_u32(const std::uint8_t *data) {
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
        (static_cast<std::uint32_t>(data[1]) << 16U) |
        (static_cast<std::uint32_t>(data[2]) << 8U) |
        static_cast<std::uint32_t>(data[3]);
}

void handle_memory_command(
    std::uint16_t connection_handle, const std::uint8_t *frame,
    std::size_t frame_size, bool secure, bool busy = false) {
    if (s_memory_store == nullptr || !memory_ble_lock()) {
        return;
    }
    if (connection_handle == s_cached_memory_connection &&
        frame_size == s_cached_memory_command_size && frame != nullptr &&
        std::memcmp(frame, s_cached_memory_command.data(), frame_size) == 0) {
        queue_memory_response_locked(
            connection_handle, s_cached_memory_response,
            s_cached_memory_response_size);
        memory_ble_unlock();
        return;
    }
    s_memory_command_active = true;
    memory_ble_unlock();

    agent::MemoryBleCommand command;
    agent::MemoryBleStatus status = agent::parse_memory_ble_command(
        frame, frame_size, secure, command);
    if (status == agent::MemoryBleStatus::authentication_required &&
        frame != nullptr &&
        frame_size >= agent::memory_ble_command_header_bytes &&
        frame_size <= agent::max_memory_ble_command_bytes &&
        std::memcmp(frame, "CEMC", 4) == 0 &&
        frame[4] == agent::memory_ble_version &&
        frame[5] >= static_cast<std::uint8_t>(
            agent::MemoryBleOperation::list_page) &&
        frame[5] <= static_cast<std::uint8_t>(
            agent::MemoryBleOperation::clear)) {
        command.operation =
            static_cast<agent::MemoryBleOperation>(frame[5]);
        command.request_id = read_memory_u32(frame + 8);
    }
    if (status == agent::MemoryBleStatus::applied && busy) {
        status = agent::MemoryBleStatus::busy;
    }
    agent::MemorySnapshot snapshot;
    const agent::Error snapshot_error = s_memory_store->snapshot(snapshot);
    agent::MemoryBleResponse response;
    response.status = status;
    response.operation = command.operation;
    response.request_id = command.request_id;
    if (snapshot_error == agent::Error::none) {
        response.revision = snapshot.revision;
        response.fingerprint = snapshot.fingerprint;
        response.total_count = static_cast<std::uint8_t>(snapshot.size);
    } else {
        response.status = agent::MemoryBleStatus::storage_failure;
    }

    if (status == agent::MemoryBleStatus::applied &&
        snapshot_error == agent::Error::none) {
        const bool initial_list = command.operation ==
                agent::MemoryBleOperation::list_page &&
            command.memory_id == 0 && command.expected_revision == 0 &&
            all_zero(command.expected_fingerprint);
        const bool version_matches = initial_list ||
            (command.expected_revision == snapshot.revision &&
             agent::memory_fingerprints_equal(
                 command.expected_fingerprint, snapshot.fingerprint));
        if (!version_matches) {
            response.status = agent::MemoryBleStatus::revision_conflict;
        } else if (command.operation == agent::MemoryBleOperation::list_page) {
            for (std::size_t index = 0; index < snapshot.size; ++index) {
                if (snapshot.entries[index].id > command.memory_id) {
                    response.memory_id = snapshot.entries[index].id;
                    response.fact = snapshot.entries[index].fact;
                    response.has_item = true;
                    response.has_more = index + 1 < snapshot.size;
                    break;
                }
            }
        } else {
            agent::MemoryMutationResult mutation;
            agent::Error error = agent::Error::tool_failed;
            if (command.operation == agent::MemoryBleOperation::add) {
                error = s_memory_store->add_from_ble(
                    command.fact.data(), command.fact.size(),
                    command.expected_revision, command.expected_fingerprint,
                    mutation);
            } else if (command.operation == agent::MemoryBleOperation::forget) {
                error = s_memory_store->forget_from_ble(
                    command.memory_id, command.expected_revision,
                    command.expected_fingerprint, mutation);
            } else if (command.operation == agent::MemoryBleOperation::clear) {
                error = s_memory_store->clear_from_ble(
                    command.expected_revision, command.expected_fingerprint,
                    mutation);
            }
            response.status = error == agent::Error::none
                ? memory_ble_status(mutation.status)
                : agent::MemoryBleStatus::storage_failure;
            response.memory_id = mutation.id;
            response.revision = mutation.revision;
            response.fingerprint = mutation.fingerprint;
            response.total_count = static_cast<std::uint8_t>(mutation.count);
        }
    }

    std::array<std::uint8_t, agent::max_memory_ble_response_bytes> encoded{};
    std::size_t encoded_size = 0;
    if (!agent::encode_memory_ble_response(response, encoded, encoded_size)) {
        response = {};
        response.status = agent::MemoryBleStatus::invalid_field;
        response.operation = command.operation;
        response.request_id = command.request_id;
        (void)agent::encode_memory_ble_response(
            response, encoded, encoded_size);
    }
    if (!memory_ble_lock()) {
        s_memory_command_active = false;
        return;
    }
    s_memory_command_active = false;
    if (s_memory_change_pending &&
        s_pending_memory_change.revision == response.revision &&
        agent::memory_fingerprints_equal(
            s_pending_memory_change.fingerprint, response.fingerprint)) {
        s_memory_change_pending = false;
    }
    if (frame != nullptr && frame_size <= s_cached_memory_command.size()) {
        s_cached_memory_command.fill(0);
        std::memcpy(s_cached_memory_command.data(), frame, frame_size);
        s_cached_memory_command_size = frame_size;
        s_cached_memory_response = encoded;
        s_cached_memory_response_size = encoded_size;
        s_cached_memory_connection = connection_handle;
    }
    queue_memory_response_locked(connection_handle, encoded, encoded_size);
    memory_ble_unlock();
}

provisioning::SessionResult authentication_acknowledgement() {
    return {
        true,
        provisioning::make_acknowledgement(
            provisioning::AcknowledgementStatus::authentication_required,
            0,
            {}),
    };
}

void release_connection(std::uint16_t connection_handle) {
    crash_diagnostics::mark(runtime::CrashEvent::ble_disconnected);
    if (s_owner_connection == connection_handle) {
        s_session.disconnect();
        s_owner_connection = kNoConnection;
    }
    if (memory_ble_lock()) {
        if (s_active_connection == connection_handle) {
            s_active_connection = kNoConnection;
            s_memory_indication_in_flight.store(
                false, std::memory_order_release);
            s_memory_change_pending = false;
            s_pending_memory_change.clear();
            s_queued_memory_response.fill(0);
            s_queued_memory_response_size = 0;
            s_queued_memory_connection = kNoConnection;
            s_cached_memory_command.fill(0);
            s_cached_memory_command_size = 0;
            s_cached_memory_response.fill(0);
            s_cached_memory_response_size = 0;
            s_cached_memory_connection = kNoConnection;
        }
        memory_ble_unlock();
    }
    if (s_acknowledgement_retry_initialized) {
        ble_npl_callout_stop(&s_acknowledgement_retry_callout);
    }
    (void)s_acknowledgement_gate.clear_connection(connection_handle);
    sync_acknowledgement_state();
    s_http_proxy_subscribed.store(false, std::memory_order_release);
    s_http_proxy_secure.store(false, std::memory_order_release);
    if (s_http_proxy_response_queue != nullptr) {
        xQueueReset(s_http_proxy_response_queue);
        HttpProxyFrame disconnected{};
        (void)xQueueSend(s_http_proxy_response_queue, &disconnected, 0);
    }
    hide_passkey(connection_handle);
}

void complete_disconnect_wait(std::uint16_t connection_handle) {
    std::uint16_t expected = connection_handle;
    if (s_disconnect_wait_connection.compare_exchange_strong(
            expected, kNoConnection, std::memory_order_acq_rel) &&
        s_disconnect_done != nullptr) {
        xSemaphoreGive(s_disconnect_done);
    }
}

void end_insecure_connection(std::uint16_t connection_handle) {
    const int result = ble_gap_terminate(
        connection_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (result == BLE_HS_ENOTCONN) {
        release_connection(connection_handle);
    } else if (result != 0 && result != BLE_HS_EALREADY) {
        release_connection(connection_handle);
        ESP_LOGW(
            kLogTag, "BLE insecure-link close failed (category %d)",
            result);
    }
}

bool advertise_once() {
    ble_hs_adv_fields advertisement{};
    advertisement.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    advertisement.uuids128 = const_cast<ble_uuid128_t *>(&kServiceUuid);
    advertisement.num_uuids128 = 1;
    advertisement.uuids128_is_complete = 1;
    int result = ble_gap_adv_set_fields(&advertisement);
    if (result != 0) {
        crash_diagnostics::mark(runtime::CrashEvent::ble_advertise_failed);
        ESP_LOGW(
            kLogTag, "BLE advertisement fields failed (category %d)", result);
        return false;
    }

    ble_hs_adv_fields response{};
    response.name = reinterpret_cast<std::uint8_t *>(
        const_cast<char *>(kDeviceName));
    response.name_len = sizeof(kDeviceName) - 1;
    response.name_is_complete = 1;
    result = ble_gap_adv_rsp_set_fields(&response);
    if (result != 0) {
        crash_diagnostics::mark(runtime::CrashEvent::ble_advertise_failed);
        ESP_LOGW(
            kLogTag, "BLE scan response failed (category %d)", result);
        return false;
    }

    ble_gap_adv_params parameters{};
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    result = ble_gap_adv_start(
        s_address_type, nullptr, BLE_HS_FOREVER, &parameters, gap_event, nullptr);
    if (result != 0 && result != BLE_HS_EALREADY) {
        crash_diagnostics::mark(runtime::CrashEvent::ble_advertise_failed);
        ESP_LOGW(
            kLogTag, "BLE advertising start failed (category %d)", result);
        return false;
    }
    crash_diagnostics::mark(runtime::CrashEvent::ble_advertise_restarted);
    return true;
}

void schedule_advertise_retry() {
    if (!s_running.load(std::memory_order_acquire) ||
        s_stop_gate.running() || !s_advertise_retry_initialized ||
        s_advertise_recovery_requested.load(std::memory_order_acquire)) {
        return;
    }
    if (s_advertise_retry_count >= kAdvertiseRetryLimit) {
        crash_diagnostics::mark(
            runtime::CrashEvent::ble_advertise_recovery);
        s_advertise_recovery_requested.store(
            true, std::memory_order_release);
        ESP_LOGE(
            kLogTag,
            "BLE advertising retries ended; host recovery requested");
        return;
    }
    ++s_advertise_retry_count;
    crash_diagnostics::mark(runtime::CrashEvent::ble_advertise_retry);
    const ble_npl_error_t result = ble_npl_callout_reset(
        &s_advertise_retry_callout,
        ble_npl_time_ms_to_ticks32(kAdvertiseRetryMs));
    if (result != BLE_NPL_OK) {
        crash_diagnostics::mark(
            runtime::CrashEvent::ble_advertise_recovery);
        s_advertise_recovery_requested.store(
            true, std::memory_order_release);
        ESP_LOGE(
            kLogTag,
            "BLE advertising retry timer failed; host recovery requested");
    }
}

void advertise_or_retry() {
    if (!s_running.load(std::memory_order_acquire) ||
        s_stop_gate.running() ||
        s_advertise_recovery_requested.load(std::memory_order_acquire)) {
        return;
    }
    if (advertise_once()) {
        s_advertise_retry_count = 0;
        ble_npl_callout_stop(&s_advertise_retry_callout);
        return;
    }
    schedule_advertise_retry();
}

void advertise_retry_event(ble_npl_event *) {
    advertise_or_retry();
}

void acknowledgement_retry_event(ble_npl_event *) {
    if (!s_running.load(std::memory_order_acquire) ||
        s_stop_gate.running()) {
        return;
    }
    try_send_acknowledgement();
    if (memory_ble_lock()) {
        try_send_memory_indication_locked();
        memory_ble_unlock();
    }
}

void deinitialize_acknowledgement_retry() {
    if (!s_acknowledgement_retry_initialized) {
        return;
    }
    ble_npl_callout_stop(&s_acknowledgement_retry_callout);
    ble_npl_callout_deinit(&s_acknowledgement_retry_callout);
    s_acknowledgement_retry_initialized = false;
}

void deinitialize_advertise_retry() {
    if (s_advertise_retry_initialized) {
        ble_npl_callout_stop(&s_advertise_retry_callout);
        ble_npl_callout_deinit(&s_advertise_retry_callout);
        s_advertise_retry_initialized = false;
    }
    s_advertise_retry_count = 0;
    s_advertise_recovery_requested.store(false, std::memory_order_release);
}

void on_sync() {
    if (ble_hs_util_ensure_addr(0) != 0 ||
        ble_hs_id_infer_auto(0, &s_address_type) != 0) {
        return;
    }
    advertise_or_retry();
}

void on_reset(int) {
    if (s_advertise_retry_initialized) {
        ble_npl_callout_stop(&s_advertise_retry_callout);
    }
    if (s_acknowledgement_retry_initialized) {
        ble_npl_callout_stop(&s_acknowledgement_retry_callout);
    }
    s_advertise_retry_count = 0;
    std::uint16_t active_connection = kNoConnection;
    s_session.disconnect();
    s_owner_connection = kNoConnection;
    if (memory_ble_lock()) {
        active_connection = s_active_connection;
        s_active_connection = kNoConnection;
        s_memory_indication_in_flight.store(false, std::memory_order_release);
        s_memory_command_active = false;
        s_memory_change_pending = false;
        s_queued_memory_response_size = 0;
        s_cached_memory_command_size = 0;
        s_cached_memory_response_size = 0;
        memory_ble_unlock();
    }
    s_acknowledgement_gate.reset_delivery();
    sync_acknowledgement_state();
    s_indication_send_active.store(false, std::memory_order_release);
    s_http_proxy_subscribed.store(false, std::memory_order_release);
    s_http_proxy_secure.store(false, std::memory_order_release);
    hide_all_passkeys();
    complete_disconnect_wait(active_connection);
}

void host_task(void *) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t disconnect_active_link() {
    std::uint16_t connection_handle = kNoConnection;
    if (!memory_ble_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    connection_handle = s_active_connection;
    memory_ble_unlock();
    if (connection_handle == kNoConnection) {
        return ESP_OK;
    }
    if (s_disconnect_done == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    (void)xSemaphoreTake(s_disconnect_done, 0);
    s_disconnect_wait_connection.store(
        connection_handle, std::memory_order_release);
    crash_diagnostics::mark(runtime::CrashEvent::ble_disconnect_requested);
    const int result = ble_gap_terminate(
        connection_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (result == BLE_HS_ENOTCONN) {
        release_connection(connection_handle);
        complete_disconnect_wait(connection_handle);
    } else if (result != 0 && result != BLE_HS_EALREADY) {
        s_disconnect_wait_connection.store(
            kNoConnection, std::memory_order_release);
        ESP_LOGW(
            kLogTag, "BLE shutdown disconnect failed (category %d)", result);
        return ESP_FAIL;
    }

    if (xSemaphoreTake(
            s_disconnect_done,
            pdMS_TO_TICKS(kDisconnectTimeoutMs)) != pdTRUE) {
        std::uint16_t expected = connection_handle;
        (void)s_disconnect_wait_connection.compare_exchange_strong(
            expected, kNoConnection, std::memory_order_acq_rel);
        crash_diagnostics::mark(runtime::CrashEvent::ble_disconnect_timeout);
        return ESP_ERR_TIMEOUT;
    }
    crash_diagnostics::mark(runtime::CrashEvent::ble_disconnect_complete);
    return ESP_OK;
}

esp_err_t stop_host() {
    if (!s_running.load(std::memory_order_acquire)) {
        return ESP_OK;
    }
    if (s_shutdown.step() == runtime::BleShutdown::Step::stop_host) {
        if (s_advertise_retry_initialized) {
            ble_npl_callout_stop(&s_advertise_retry_callout);
        }
        if (s_acknowledgement_retry_initialized) {
            ble_npl_callout_stop(&s_acknowledgement_retry_callout);
        }
        (void)ble_gap_adv_stop();
        const esp_err_t disconnect_result = disconnect_active_link();
        if (disconnect_result != ESP_OK) {
            return disconnect_result;
        }
        crash_diagnostics::mark(runtime::CrashEvent::ble_host_stop_begin);
        if (nimble_port_stop() != 0) {
            return ESP_FAIL;
        }
        deinitialize_advertise_retry();
        deinitialize_acknowledgement_retry();
        crash_diagnostics::mark(runtime::CrashEvent::ble_host_stop_complete);
        crash_diagnostics::mark(runtime::CrashEvent::ble_store_capture_begin);
        if (!capture_volatile_store()) {
            ESP_LOGE(kLogTag, "Could not retain the volatile BLE bond store");
        }
        crash_diagnostics::mark(
            runtime::CrashEvent::ble_store_capture_complete);
        s_shutdown.host_stopped();
    }
    if (s_shutdown.step() ==
        runtime::BleShutdown::Step::deinitialize_host) {
        crash_diagnostics::mark(runtime::CrashEvent::ble_deinit_begin);
        const esp_err_t deinit_result = nimble_port_deinit();
        if (deinit_result != ESP_OK) {
            return deinit_result;
        }
        crash_diagnostics::mark(runtime::CrashEvent::ble_deinit_complete);
        s_shutdown.host_deinitialized();
    }
    if (s_memory_store != nullptr) {
        s_memory_store->set_change_callback(nullptr, nullptr);
    }
    s_session.disconnect();
    s_owner_connection = kNoConnection;
    hide_all_passkeys();
    s_acknowledgement_gate.reset();
    s_acknowledgement_waiting.store(false, std::memory_order_release);
    s_acknowledgement_in_flight.store(false, std::memory_order_release);
    s_settings_confirmation_pending.store(false, std::memory_order_release);
    s_indication_send_active.store(false, std::memory_order_release);
    s_memory_indication_in_flight.store(false, std::memory_order_release);
    s_http_proxy_subscribed.store(false, std::memory_order_release);
    s_http_proxy_secure.store(false, std::memory_order_release);
    if (s_memory_ble_mutex != nullptr) {
        vSemaphoreDelete(s_memory_ble_mutex);
        s_memory_ble_mutex = nullptr;
    }
    if (s_disconnect_done != nullptr) {
        vSemaphoreDelete(s_disconnect_done);
        s_disconnect_done = nullptr;
    }
    delete_http_proxy_resources();
    s_disconnect_wait_connection.store(
        kNoConnection, std::memory_order_release);
    s_pending_memory_change.clear();
    s_queued_memory_response.fill(0);
    s_queued_memory_response_size = 0;
    s_cached_memory_command.fill(0);
    s_cached_memory_command_size = 0;
    s_cached_memory_response.fill(0);
    s_cached_memory_response_size = 0;
    s_settings_store = nullptr;
    s_memory_store = nullptr;
    s_passkey_callback = nullptr;
    s_device_context_callback = nullptr;
    s_callback_context = nullptr;
    s_running.store(false, std::memory_order_release);
    return ESP_OK;
}

void stop_task(void *) {
    crash_diagnostics::mark(runtime::CrashEvent::ble_stop_task_start);
    const bool watchdog_active = esp_task_wdt_add(nullptr) == ESP_OK;
    const esp_err_t result = stop_host();
    crash_diagnostics::mark(runtime::CrashEvent::ble_stop_complete);
    ESP_LOGI(
        kLogTag, "BLE stop stack minimum free bytes: %u",
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    resource_telemetry::record_task_watermark(task_config::TaskId::ble_stop);
    if (watchdog_active) {
        (void)esp_task_wdt_delete(nullptr);
    }
    s_stop_result.store(result, std::memory_order_release);
    s_stop_gate.complete();
    if (s_stop_done != nullptr) {
        xSemaphoreGive(s_stop_done);
    }
    vTaskDelete(nullptr);
}

esp_err_t consume_stop_result() {
    if (!s_stop_gate.consume_completion()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_stop_done != nullptr) {
        (void)xSemaphoreTake(s_stop_done, 0);
    }
    const esp_err_t result =
        s_stop_result.load(std::memory_order_acquire);
    // The worker deletes itself after it gives the semaphore. Give the idle
    // task one bounded interval to reclaim that stack before the caller starts
    // another memory-heavy worker or initializes Bluetooth again.
    vTaskDelay(pdMS_TO_TICKS(kStopTaskReclaimDelayMs));
    s_lifecycle.store(LifecycleState::idle, std::memory_order_release);
    return result;
}

void log_bond_state(std::uint16_t connection_handle) {
    ble_gap_conn_desc description{};
    if (ble_gap_conn_find(connection_handle, &description) != 0) {
        ESP_LOGI(kLogTag, "BLE bond state unavailable");
        return;
    }
    ble_store_key_sec key{};
    key.peer_addr = description.peer_id_addr;
    ble_store_value_sec local_security{};
    ble_store_value_sec peer_security{};
    const bool local_bond =
        ble_store_read_our_sec(&key, &local_security) == 0;
    const bool peer_bond =
        ble_store_read_peer_sec(&key, &peer_security) == 0;
    ESP_LOGI(
        kLogTag, "BLE bond state: local=%d peer=%d",
        local_bond ? 1 : 0, peer_bond ? 1 : 0);
}

int gap_event(ble_gap_event *event, void *) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                crash_diagnostics::mark(runtime::CrashEvent::ble_connected);
                if (memory_ble_lock()) {
                    s_active_connection = event->connect.conn_handle;
                    memory_ble_unlock();
                }
                log_bond_state(event->connect.conn_handle);
                ESP_LOGI(
                    kLogTag,
                    "BLE pairing memory: free=%u largest=%u",
                    static_cast<unsigned>(heap_caps_get_free_size(
                        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)),
                    static_cast<unsigned>(heap_caps_get_largest_free_block(
                        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)));
                const int security_result =
                    ble_gap_security_initiate(event->connect.conn_handle);
                ESP_LOGI(
                    kLogTag, "BLE security start result: category %d",
                    security_result);
                if (security_result != 0 &&
                    security_result != BLE_HS_EALREADY) {
                    crash_diagnostics::mark(
                        runtime::CrashEvent::ble_security_start_failed);
                    ESP_LOGW(
                        kLogTag,
                        "BLE security start failed (category %d)",
                        security_result);
                    end_insecure_connection(event->connect.conn_handle);
                }
            } else {
                crash_diagnostics::mark(
                    runtime::CrashEvent::ble_connection_failed);
                ESP_LOGW(
                    kLogTag, "BLE connection failed (category %d)",
                    event->connect.status);
                if (!s_stop_gate.running()) {
                    advertise_or_retry();
                }
            }
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGW(
                kLogTag, "BLE link ended (category %d)",
                event->disconnect.reason);
            release_connection(event->disconnect.conn.conn_handle);
            if (!s_stop_gate.running()) {
                advertise_or_retry();
            }
            complete_disconnect_wait(event->disconnect.conn.conn_handle);
            return 0;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            if (!s_stop_gate.running()) {
                advertise_or_retry();
            }
            return 0;
        case BLE_GAP_EVENT_ENC_CHANGE:
            hide_passkey(event->enc_change.conn_handle);
            if (provisioning::link_is_secure(
                    link_security(event->enc_change.conn_handle))) {
                crash_diagnostics::mark(runtime::CrashEvent::ble_secure);
                s_bond_available.store(true, std::memory_order_release);
                s_http_proxy_secure.store(true, std::memory_order_release);
                ESP_LOGI(kLogTag, "BLE link is secure");
            } else {
                crash_diagnostics::mark(
                    runtime::CrashEvent::ble_security_failed);
                ESP_LOGW(
                    kLogTag, "BLE security failed (category %d)",
                    event->enc_change.status);
                if (s_stop_gate.running()) {
                    ESP_LOGI(
                        kLogTag,
                        "BLE security ended during an intentional stop");
                    return 0;
                }
                // The product supports one phone. If security fails, a saved
                // bond can only be stale or incomplete. Remove it so the next
                // connection can create a complete identity-key bond instead
                // of repeating an authentication failure forever.
                (void)remove_stored_bond(inspect_bond_store());
                end_insecure_connection(event->enc_change.conn_handle);
            }
            return 0;
        case BLE_GAP_EVENT_PASSKEY_ACTION: {
            if (event->passkey.params.action != BLE_SM_IOACT_DISP) {
                return BLE_HS_ENOTSUP;
            }
            if (s_passkey_connection != kNoConnection &&
                s_passkey_connection != event->passkey.conn_handle) {
                return BLE_HS_EBUSY;
            }
            const std::uint32_t passkey =
                100000U + esp_random() % 900000U;
            s_passkey_connection = event->passkey.conn_handle;
            show_passkey(passkey, true);
            ble_sm_io input{};
            input.action = BLE_SM_IOACT_DISP;
            input.passkey = passkey;
            const int result =
                ble_sm_inject_io(event->passkey.conn_handle, &input);
            if (result != 0) {
                hide_passkey(event->passkey.conn_handle);
            }
            return result;
        }
        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            crash_diagnostics::mark(
                runtime::CrashEvent::ble_repeat_pairing);
            ESP_LOGW(kLogTag, "BLE peer requested a new pairing");
            s_bond_available.store(false, std::memory_order_release);
            ble_gap_conn_desc description{};
            if (ble_gap_conn_find(
                    event->repeat_pairing.conn_handle, &description) != 0) {
                return BLE_GAP_REPEAT_PAIRING_IGNORE;
            }
            if (ble_store_util_delete_peer(&description.peer_id_addr) != 0) {
                return BLE_GAP_REPEAT_PAIRING_IGNORE;
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        case BLE_GAP_EVENT_NOTIFY_TX: {
            if (event->notify_tx.indication == 0 ||
                event->notify_tx.status == 0) {
                return 0;
            }
            if (event->notify_tx.attr_handle == s_memory_response_handle ||
                event->notify_tx.attr_handle == s_acknowledgement_handle) {
                ESP_LOGI(
                    kLogTag,
                    "BLE indication completed: channel=%s category=%d",
                    event->notify_tx.attr_handle == s_memory_response_handle
                            ? "memory"
                            : "acknowledgement",
                    event->notify_tx.status);
            }
            // NimBLE reports an immediate send error from inside
            // ble_gatts_indicate_custom(). Do not re-enter either sender while
            // its state and the memory mutex still belong to that call.
            if (s_indication_send_active.load(std::memory_order_acquire)) {
                return 0;
            }
            if (event->notify_tx.attr_handle == s_acknowledgement_handle) {
                const auto action = s_acknowledgement_gate.complete(
                    event->notify_tx.status == BLE_HS_EDONE);
                sync_acknowledgement_state();
                if (action == provisioning::IndicationDeliveryAction::
                        settings_confirmed) {
                    crash_diagnostics::mark(
                        runtime::CrashEvent::settings_ack_confirmed);
                } else if (
                    action == provisioning::IndicationDeliveryAction::
                            settings_retry ||
                    action == provisioning::IndicationDeliveryAction::
                            settings_failed) {
                    handle_acknowledgement_failure(action);
                    if (action == provisioning::IndicationDeliveryAction::
                            settings_retry) {
                        return 0;
                    }
                } else if (
                    action == provisioning::IndicationDeliveryAction::retry) {
                    handle_acknowledgement_failure(action);
                    return 0;
                }
            } else if (event->notify_tx.attr_handle ==
                       s_memory_response_handle) {
                s_memory_indication_in_flight.store(
                    false, std::memory_order_release);
            }
            try_send_acknowledgement();
            if (memory_ble_lock()) {
                try_send_memory_indication_locked();
                memory_ble_unlock();
            }
            return 0;
        }
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle ==
                s_http_proxy_request_handle) {
                s_http_proxy_subscribed.store(
                    event->subscribe.cur_notify != 0,
                    std::memory_order_release);
                ESP_LOGI(
                    kLogTag,
                    "BLE proxy subscription: notify=%d indicate=%d",
                    event->subscribe.cur_notify != 0 ? 1 : 0,
                    event->subscribe.cur_indicate != 0 ? 1 : 0);
            }
            try_send_acknowledgement();
            if (memory_ble_lock()) {
                try_send_memory_indication_locked();
                memory_ble_unlock();
            }
            return 0;
        default:
            return 0;
    }
}

int gatt_access(
    std::uint16_t connection_handle,
    std::uint16_t,
    ble_gatt_access_ctxt *context,
    void *argument) {
    if (context == nullptr || context->op != BLE_GATT_ACCESS_OP_WRITE_CHR ||
        s_settings_store == nullptr) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    const provisioning::LinkSecurity security =
        link_security(connection_handle);
    const std::uintptr_t characteristic =
        reinterpret_cast<std::uintptr_t>(argument);
    if (!provisioning::link_is_secure(security)) {
        if (s_owner_connection == connection_handle) {
            release_connection(connection_handle);
        }
        if (characteristic == 5) {
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        } else if (characteristic == 4) {
            const std::size_t frame_size = OS_MBUF_PKTLEN(context->om);
            std::array<std::uint8_t, agent::max_memory_ble_command_bytes> frame{};
            std::uint16_t copied_size = 0;
            if (frame_size <= frame.size() &&
                ble_hs_mbuf_to_flat(
                    context->om, frame.data(), frame.size(), &copied_size) == 0 &&
                copied_size == frame_size) {
                handle_memory_command(
                    connection_handle, frame.data(), copied_size, false);
            } else {
                handle_memory_command(
                    connection_handle, nullptr, frame_size, false);
            }
            clear_bytes(frame.data(), frame.size());
        } else if (characteristic == 3) {
            send_device_context_acknowledgement(
                connection_handle,
                provisioning::make_device_context_acknowledgement(
                    provisioning::DeviceContextStatus::authentication_required));
        } else {
            send_acknowledgement(
                connection_handle, authentication_acknowledgement());
        }
        return 0;
    }

    if (characteristic == 5) {
        const std::size_t frame_size = OS_MBUF_PKTLEN(context->om);
        if (frame_size == 0 || frame_size > kMaximumHttpProxyFrameSize ||
            s_http_proxy_response_queue == nullptr) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        HttpProxyFrame frame{};
        std::uint16_t copied_size = 0;
        if (ble_hs_mbuf_to_flat(
                context->om, frame.data.data(), frame.data.size(),
                &copied_size) != 0 || copied_size != frame_size) {
            return BLE_ATT_ERR_UNLIKELY;
        }
        frame.size = copied_size;
        if (xQueueSend(s_http_proxy_response_queue, &frame, 0) != pdTRUE) {
            clear_bytes(frame.data.data(), frame.data.size());
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        clear_bytes(frame.data.data(), frame.data.size());
        return 0;
    }

    if (characteristic == 4) {
        crash_diagnostics::mark(runtime::CrashEvent::memory_command_begin);
        const std::size_t frame_size = OS_MBUF_PKTLEN(context->om);
        std::array<std::uint8_t, agent::max_memory_ble_command_bytes> frame{};
        std::uint16_t copied_size = 0;
        if (frame_size <= frame.size() &&
            ble_hs_mbuf_to_flat(
                context->om, frame.data(), frame.size(), &copied_size) == 0 &&
            copied_size == frame_size) {
            handle_memory_command(
                connection_handle, frame.data(), copied_size, true,
                s_owner_connection != kNoConnection);
        } else {
            handle_memory_command(connection_handle, nullptr, frame_size, true);
        }
        clear_bytes(frame.data(), frame.size());
        ESP_LOGI(
            kLogTag, "NimBLE host stack minimum free bytes: %u",
            static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        return 0;
    }


    if (characteristic == 3) {
        if (s_owner_connection != kNoConnection) {
            send_device_context_acknowledgement(
                connection_handle,
                provisioning::make_device_context_acknowledgement(
                    provisioning::DeviceContextStatus::busy));
            return 0;
        }
        const std::size_t frame_size = OS_MBUF_PKTLEN(context->om);
        std::array<std::uint8_t, provisioning::kMaximumDeviceContextSize> frame{};
        std::uint16_t copied_size = 0;
        provisioning::DeviceContextResult result;
        if (frame_size <= frame.size() &&
            ble_hs_mbuf_to_flat(
                context->om, frame.data(), frame.size(), &copied_size) == 0 &&
            copied_size == frame_size) {
            result = provisioning::validate_device_context_packet(
                frame.data(), copied_size, security);
        }
        if (result.valid() && s_device_context_callback != nullptr) {
            s_device_context_callback(
                result.epoch_seconds,
                result.utc_offset_minutes,
                result.approximate_location.data(),
                result.approximate_location.size(),
                s_callback_context);
        }
        send_device_context_acknowledgement(
            connection_handle,
            provisioning::make_device_context_acknowledgement(
                result.status, result.epoch_seconds, result.fingerprint));
        clear_bytes(frame.data(), frame.size());
        return 0;
    }

    if (s_owner_connection != kNoConnection &&
        s_owner_connection != connection_handle) {
        send_acknowledgement(
            connection_handle, s_session.busy_acknowledgement());
        return 0;
    }
    s_owner_connection = connection_handle;

    const std::size_t maximum_size = characteristic == 1
        ? provisioning::kControlFrameSize
        : kMaximumDataFrameSize;
    const std::size_t frame_size = OS_MBUF_PKTLEN(context->om);
    if (frame_size > maximum_size) {
        const auto result = characteristic == 1
            ? s_session.handle_control(nullptr, frame_size, security)
            : s_session.handle_data(
                  nullptr, frame_size, security, *s_settings_store);
        send_acknowledgement(connection_handle, result);
        if (!s_session.active()) {
            s_owner_connection = kNoConnection;
        }
        return 0;
    }

    std::array<std::uint8_t, kMaximumDataFrameSize> frame{};
    std::uint16_t copied_size = 0;
    if (ble_hs_mbuf_to_flat(
            context->om, frame.data(), maximum_size, &copied_size) != 0 ||
        copied_size != frame_size) {
        clear_bytes(frame.data(), frame.size());
        s_session.disconnect();
        s_owner_connection = kNoConnection;
        return BLE_ATT_ERR_UNLIKELY;
    }

    const provisioning::SessionResult result = characteristic == 1
        ? s_session.handle_control(frame.data(), copied_size, security)
        : s_session.handle_data(
              frame.data(), copied_size, security, *s_settings_store);
    clear_bytes(frame.data(), frame.size());
    if (characteristic == 1 && s_session.active()) {
        crash_diagnostics::mark(
            runtime::CrashEvent::settings_transfer_begin);
    } else if (characteristic == 2 && result.has_acknowledgement) {
        crash_diagnostics::mark(
            runtime::CrashEvent::settings_packet_complete);
    }
    send_acknowledgement(connection_handle, result);
    if (!s_session.active()) {
        s_owner_connection = kNoConnection;
    }
    return 0;
}

}  // namespace

esp_err_t start(
    SettingsStore *settings_store,
    DeviceMemoryStore *memory_store,
    PasskeyCallback passkey_callback,
    DeviceContextCallback device_context_callback,
    void *callback_context) {
    if (settings_store == nullptr || memory_store == nullptr ||
        passkey_callback == nullptr ||
        device_context_callback == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_stop_gate.completed()) {
        const esp_err_t prior_stop_result = consume_stop_result();
        if (s_running.load(std::memory_order_acquire)) {
            return prior_stop_result;
        }
    }
    LifecycleState expected = LifecycleState::idle;
    if (!s_lifecycle.compare_exchange_strong(
            expected, LifecycleState::starting,
            std::memory_order_acq_rel)) {
        return ESP_ERR_INVALID_STATE;
    }
    StartGateGuard start_gate(true);
    if (s_stop_gate.running()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_running.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t storage_result = settings_store->initialize();
    if (storage_result != ESP_OK) {
        return storage_result;
    }
#if !CHATESP_DEVELOPMENT_MODE && \
    defined(CONFIG_BT_NIMBLE_NVS_PERSIST) && CONFIG_BT_NIMBLE_NVS_PERSIST
    if (settings_store->persistence() != SettingsPersistence::plaintext_nvs) {
        return ESP_ERR_INVALID_STATE;
    }
#endif

    s_settings_store = settings_store;
    s_memory_store = memory_store;
    s_passkey_callback = passkey_callback;
    s_device_context_callback = device_context_callback;
    s_callback_context = callback_context;
    s_session.disconnect();
    s_owner_connection = kNoConnection;
    s_acknowledgement_gate.reset();
    s_acknowledgement_waiting.store(false, std::memory_order_release);
    s_acknowledgement_in_flight.store(false, std::memory_order_release);
    s_settings_confirmation_pending.store(false, std::memory_order_release);
    s_indication_send_active.store(false, std::memory_order_release);
    s_memory_ble_mutex = xSemaphoreCreateMutex();
    if (s_memory_ble_mutex == nullptr) {
        s_settings_store = nullptr;
        s_memory_store = nullptr;
        return ESP_ERR_NO_MEM;
    }
    s_disconnect_done = xSemaphoreCreateBinary();
    if (s_disconnect_done == nullptr) {
        vSemaphoreDelete(s_memory_ble_mutex);
        s_memory_ble_mutex = nullptr;
        s_settings_store = nullptr;
        s_memory_store = nullptr;
        return ESP_ERR_NO_MEM;
    }
    s_http_proxy_send_mutex = xSemaphoreCreateMutex();
    s_http_proxy_response_queue = xQueueCreate(8, sizeof(HttpProxyFrame));
    if (s_http_proxy_send_mutex == nullptr ||
        s_http_proxy_response_queue == nullptr) {
        delete_http_proxy_resources();
        vSemaphoreDelete(s_disconnect_done);
        s_disconnect_done = nullptr;
        vSemaphoreDelete(s_memory_ble_mutex);
        s_memory_ble_mutex = nullptr;
        s_settings_store = nullptr;
        s_memory_store = nullptr;
        return ESP_ERR_NO_MEM;
    }
    s_disconnect_wait_connection.store(
        kNoConnection, std::memory_order_release);
    s_active_connection = kNoConnection;
    s_memory_response_handle = 0;
    s_http_proxy_request_handle = 0;
    s_http_proxy_subscribed.store(false, std::memory_order_release);
    s_http_proxy_secure.store(false, std::memory_order_release);
    s_memory_indication_in_flight = false;
    s_memory_command_active = false;
    s_memory_change_pending = false;
    s_queued_memory_response.fill(0);
    s_queued_memory_response_size = 0;
    s_queued_memory_connection = kNoConnection;
    s_cached_memory_command.fill(0);
    s_cached_memory_command_size = 0;
    s_cached_memory_response.fill(0);
    s_cached_memory_response_size = 0;
    s_cached_memory_connection = kNoConnection;
    s_memory_store->set_change_callback(memory_changed_callback, nullptr);

    const esp_err_t init_result = nimble_port_init();
    if (init_result != ESP_OK) {
        s_memory_store->set_change_callback(nullptr, nullptr);
        delete_http_proxy_resources();
        vSemaphoreDelete(s_memory_ble_mutex);
        s_memory_ble_mutex = nullptr;
        vSemaphoreDelete(s_disconnect_done);
        s_disconnect_done = nullptr;
        s_settings_store = nullptr;
        s_memory_store = nullptr;
        s_passkey_callback = nullptr;
        s_device_context_callback = nullptr;
        s_callback_context = nullptr;
        return init_result;
    }

    s_advertise_retry_count = 0;
    s_advertise_recovery_requested.store(false, std::memory_order_release);
    if (ble_npl_callout_init(
            &s_advertise_retry_callout,
            nimble_port_get_dflt_eventq(), advertise_retry_event,
            nullptr) != 0) {
        nimble_port_deinit();
        s_memory_store->set_change_callback(nullptr, nullptr);
        delete_http_proxy_resources();
        vSemaphoreDelete(s_memory_ble_mutex);
        s_memory_ble_mutex = nullptr;
        vSemaphoreDelete(s_disconnect_done);
        s_disconnect_done = nullptr;
        s_settings_store = nullptr;
        s_memory_store = nullptr;
        s_passkey_callback = nullptr;
        s_device_context_callback = nullptr;
        s_callback_context = nullptr;
        return ESP_ERR_NO_MEM;
    }
    s_advertise_retry_initialized = true;

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_sc_only = 1;
    ble_hs_cfg.sm_sec_lvl = 4;
    // Keep the peer identity key with the encryption key. iOS uses a
    // resolvable private address, so an encryption-only bond cannot be found
    // after the phone changes its over-the-air address.
    ble_hs_cfg.sm_our_key_dist =
        BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist =
        BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int result = ble_gatts_count_cfg(s_services);
    if (result == 0) {
        result = ble_gatts_add_svcs(s_services);
    }
    if (result == 0) {
        result = ble_svc_gap_device_name_set(kDeviceName);
    }
    if (result != 0) {
        deinitialize_advertise_retry();
        nimble_port_deinit();
        s_memory_store->set_change_callback(nullptr, nullptr);
        delete_http_proxy_resources();
        vSemaphoreDelete(s_memory_ble_mutex);
        s_memory_ble_mutex = nullptr;
        vSemaphoreDelete(s_disconnect_done);
        s_disconnect_done = nullptr;
        s_settings_store = nullptr;
        s_memory_store = nullptr;
        s_passkey_callback = nullptr;
        s_device_context_callback = nullptr;
        s_callback_context = nullptr;
        return ESP_FAIL;
    }

    ble_store_config_init();
    if (!restore_volatile_store()) {
        ESP_LOGE(kLogTag, "Could not restore the volatile BLE bond store");
        deinitialize_advertise_retry();
        nimble_port_deinit();
        s_memory_store->set_change_callback(nullptr, nullptr);
        delete_http_proxy_resources();
        vSemaphoreDelete(s_memory_ble_mutex);
        s_memory_ble_mutex = nullptr;
        vSemaphoreDelete(s_disconnect_done);
        s_disconnect_done = nullptr;
        s_settings_store = nullptr;
        s_memory_store = nullptr;
        s_passkey_callback = nullptr;
        s_device_context_callback = nullptr;
        s_callback_context = nullptr;
        return ESP_FAIL;
    }
    BondInventory bond_inventory = inspect_bond_store();
    ESP_LOGI(
        kLogTag,
        "BLE stored bond keys: local_ltk=%d local_id=%d "
        "peer_ltk=%d peer_id=%d",
        bond_inventory.local_ltk ? 1 : 0,
        bond_inventory.local_identity ? 1 : 0,
        bond_inventory.peer_ltk ? 1 : 0,
        bond_inventory.peer_identity ? 1 : 0);
    if ((bond_inventory.local != 0 || bond_inventory.peer != 0) &&
        !complete_bond_is_stored(bond_inventory)) {
        (void)remove_stored_bond(bond_inventory);
        bond_inventory = inspect_bond_store();
    }
    s_bond_available.store(
        complete_bond_is_stored(bond_inventory), std::memory_order_release);
    ESP_LOGI(
        kLogTag, "BLE stored bond ready: %d",
        s_bond_available.load(std::memory_order_acquire) ? 1 : 0);
    if (ble_npl_callout_init(
            &s_acknowledgement_retry_callout,
            nimble_port_get_dflt_eventq(), acknowledgement_retry_event,
            nullptr) != 0) {
        deinitialize_advertise_retry();
        nimble_port_deinit();
        s_memory_store->set_change_callback(nullptr, nullptr);
        delete_http_proxy_resources();
        vSemaphoreDelete(s_memory_ble_mutex);
        s_memory_ble_mutex = nullptr;
        vSemaphoreDelete(s_disconnect_done);
        s_disconnect_done = nullptr;
        s_settings_store = nullptr;
        s_memory_store = nullptr;
        s_passkey_callback = nullptr;
        s_device_context_callback = nullptr;
        s_callback_context = nullptr;
        return ESP_ERR_NO_MEM;
    }
    s_acknowledgement_retry_initialized = true;
    s_shutdown.reset();
    s_running.store(true, std::memory_order_release);
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

esp_err_t stop(std::uint32_t timeout_ms) {
    if (s_stop_gate.completed()) {
        return consume_stop_result();
    }
    const TickType_t started = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms != 0 && timeout_ticks == 0) {
        timeout_ticks = 1;
    }
    TickType_t poll_ticks = pdMS_TO_TICKS(kLifecyclePollMs);
    if (poll_ticks == 0) {
        poll_ticks = 1;
    }

    bool owns_stop_gate = false;
    while (!s_stop_gate.running()) {
        if (s_stop_gate.completed()) {
            return consume_stop_result();
        }
        LifecycleState expected = LifecycleState::idle;
        if (s_lifecycle.compare_exchange_strong(
                expected, LifecycleState::stopping,
                std::memory_order_acq_rel)) {
            owns_stop_gate = true;
            break;
        }
        if (xTaskGetTickCount() - started >= timeout_ticks) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(poll_ticks);
    }

    if (owns_stop_gate && !s_running.load(std::memory_order_acquire)) {
        s_lifecycle.store(LifecycleState::idle, std::memory_order_release);
        return ESP_OK;
    }
    if (owns_stop_gate) {
        if (s_stop_done == nullptr) {
            s_stop_done = xSemaphoreCreateBinary();
            if (s_stop_done == nullptr) {
                s_lifecycle.store(
                    LifecycleState::idle, std::memory_order_release);
                return ESP_ERR_NO_MEM;
            }
        }
        if (!s_stop_gate.begin()) {
            s_lifecycle.store(
                LifecycleState::idle, std::memory_order_release);
            return ESP_ERR_INVALID_STATE;
        }
        crash_diagnostics::mark(runtime::CrashEvent::ble_stop_requested);
        const BaseType_t task_result = task_config::create(
            stop_task, task_config::ble_stop, nullptr, nullptr);
        if (task_result != pdPASS) {
            (void)s_stop_gate.cancel_begin();
            s_lifecycle.store(
                LifecycleState::idle, std::memory_order_release);
            return ESP_ERR_NO_MEM;
        }
    }
    const TickType_t elapsed = xTaskGetTickCount() - started;
    const TickType_t remaining =
        elapsed < timeout_ticks ? timeout_ticks - elapsed : 0;
    if (xSemaphoreTake(s_stop_done, remaining) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return consume_stop_result();
}

bool running() { return s_running.load(std::memory_order_acquire); }

bool starting() {
    return s_lifecycle.load(std::memory_order_acquire) ==
        LifecycleState::starting;
}

bool bond_available() {
    return s_bond_available.load(std::memory_order_acquire);
}

bool settings_confirmation_pending() {
    return s_settings_confirmation_pending.load(std::memory_order_acquire);
}

bool advertising_recovery_requested() {
    return s_advertise_recovery_requested.load(std::memory_order_acquire);
}

bool secure_link_connected() {
    return s_running.load(std::memory_order_acquire) &&
        s_http_proxy_secure.load(std::memory_order_acquire);
}

bool http_proxy_available() {
    if (!s_running.load(std::memory_order_acquire) ||
        !s_http_proxy_subscribed.load(std::memory_order_acquire) ||
        !s_http_proxy_secure.load(std::memory_order_acquire)) {
        return false;
    }
    if (!memory_ble_lock()) {
        return false;
    }
    const bool available = s_active_connection != kNoConnection;
    memory_ble_unlock();
    return available;
}

std::size_t http_proxy_frame_capacity() {
    if (!http_proxy_available() || !memory_ble_lock()) {
        return 0;
    }
    const std::uint16_t connection_handle = s_active_connection;
    memory_ble_unlock();
    const std::uint16_t mtu = ble_att_mtu(connection_handle);
    if (mtu <= 3) {
        return 0;
    }
    const std::size_t payload = static_cast<std::size_t>(mtu - 3);
    return payload < kMaximumHttpProxyFrameSize
        ? payload
        : kMaximumHttpProxyFrameSize;
}

void begin_http_proxy_exchange() {
    if (s_http_proxy_response_queue != nullptr) {
        xQueueReset(s_http_proxy_response_queue);
    }
}

esp_err_t send_http_proxy_frame(
    const std::uint8_t *data, std::size_t size, std::uint32_t timeout_ms) {
    if (data == nullptr || size == 0 || timeout_ms == 0 ||
        s_http_proxy_send_mutex == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(
            s_http_proxy_send_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    esp_err_t result = ESP_ERR_TIMEOUT;
    while (xTaskGetTickCount() - started < timeout_ticks) {
        if (!http_proxy_available()) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        const std::size_t capacity = http_proxy_frame_capacity();
        if (size > capacity) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        std::uint16_t connection_handle = kNoConnection;
        if (memory_ble_lock()) {
            connection_handle = s_active_connection;
            memory_ble_unlock();
        }
        if (connection_handle == kNoConnection) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        os_mbuf *packet = ble_hs_mbuf_from_flat(data, size);
        if (packet == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        const int send_result = ble_gatts_notify_custom(
            connection_handle, s_http_proxy_request_handle, packet);
        if (send_result == 0) {
            result = ESP_OK;
            break;
        }
        if (send_result == BLE_HS_ENOMEM || send_result == BLE_HS_EBUSY) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        result = ESP_FAIL;
        ESP_LOGW(
            kLogTag, "BLE proxy notification failed (category %d)",
            send_result);
        break;
    }
    if (result != ESP_OK) {
        ESP_LOGW(
            kLogTag,
            "BLE proxy frame failed: size=%u category=%s",
            static_cast<unsigned>(size), esp_err_to_name(result));
    }
    xSemaphoreGive(s_http_proxy_send_mutex);
    return result;
}

esp_err_t receive_http_proxy_frame(
    std::uint8_t *data, std::size_t capacity, std::size_t *size,
    std::uint32_t timeout_ms) {
    if (data == nullptr || size == nullptr || capacity == 0 ||
        timeout_ms == 0 || s_http_proxy_response_queue == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    HttpProxyFrame frame{};
    if (xQueueReceive(
            s_http_proxy_response_queue, &frame,
            pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (frame.size == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (frame.size > capacity) {
        clear_bytes(frame.data.data(), frame.data.size());
        return ESP_ERR_INVALID_SIZE;
    }
    std::memcpy(data, frame.data.data(), frame.size);
    *size = frame.size;
    clear_bytes(frame.data.data(), frame.data.size());
    return ESP_OK;
}

void cancel_http_proxy_exchange() {
    if (s_http_proxy_response_queue != nullptr) {
        xQueueReset(s_http_proxy_response_queue);
        HttpProxyFrame cancelled{};
        (void)xQueueSend(s_http_proxy_response_queue, &cancelled, 0);
    }
}

}  // namespace ble_provisioning
}  // namespace chatesp
