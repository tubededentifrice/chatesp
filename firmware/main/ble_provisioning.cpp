#include "ble_provisioning.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#include "chatesp/provisioning_session.hpp"
#include "esp_random.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "settings_store.hpp"

extern "C" void ble_store_config_init(void);

namespace chatesp {
namespace ble_provisioning {
namespace {

constexpr char kDeviceName[] = "ChatESP Setup";
constexpr std::uint16_t kNoConnection = BLE_HS_CONN_HANDLE_NONE;
constexpr std::size_t kMaximumDataFrameSize =
    provisioning::kDataFrameHeaderSize + provisioning::kMaximumFrameDataSize;

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

SettingsStore *s_settings_store = nullptr;
PasskeyCallback s_passkey_callback = nullptr;
DeviceContextCallback s_device_context_callback = nullptr;
void *s_callback_context = nullptr;
provisioning::ProvisioningSession s_session;
std::uint16_t s_owner_connection = kNoConnection;
std::uint16_t s_passkey_connection = kNoConnection;
std::uint16_t s_acknowledgement_handle = 0;
std::uint8_t s_address_type = 0;
bool s_running = false;

int gap_event(ble_gap_event *event, void *argument);
int gatt_access(
    std::uint16_t connection_handle,
    std::uint16_t attribute_handle,
    ble_gatt_access_ctxt *context,
    void *argument);

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
    {},
};

ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kServiceUuid.u,
        .includes = nullptr,
        .characteristics = s_characteristics,
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

void send_acknowledgement(
    std::uint16_t connection_handle,
    const provisioning::SessionResult &result) {
    if (!result.has_acknowledgement || s_acknowledgement_handle == 0) {
        return;
    }
    os_mbuf *packet = ble_hs_mbuf_from_flat(
        result.acknowledgement.data(), result.acknowledgement.size());
    if (packet == nullptr) {
        return;
    }
    ble_gatts_indicate_custom(
        connection_handle, s_acknowledgement_handle, packet);
}

void send_device_context_acknowledgement(
    std::uint16_t connection_handle,
    const std::array<std::uint8_t,
        provisioning::kDeviceContextAcknowledgementSize> &acknowledgement) {
    if (s_acknowledgement_handle == 0) {
        return;
    }
    os_mbuf *packet = ble_hs_mbuf_from_flat(
        acknowledgement.data(), acknowledgement.size());
    if (packet != nullptr) {
        ble_gatts_indicate_custom(
            connection_handle, s_acknowledgement_handle, packet);
    }
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
    if (s_owner_connection == connection_handle) {
        s_session.disconnect();
        s_owner_connection = kNoConnection;
    }
    hide_passkey(connection_handle);
}

void advertise() {
    ble_hs_adv_fields advertisement{};
    advertisement.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    advertisement.uuids128 = const_cast<ble_uuid128_t *>(&kServiceUuid);
    advertisement.num_uuids128 = 1;
    advertisement.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&advertisement) != 0) {
        return;
    }

    ble_hs_adv_fields response{};
    response.name = reinterpret_cast<std::uint8_t *>(
        const_cast<char *>(kDeviceName));
    response.name_len = sizeof(kDeviceName) - 1;
    response.name_is_complete = 1;
    if (ble_gap_adv_rsp_set_fields(&response) != 0) {
        return;
    }

    ble_gap_adv_params parameters{};
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(
        s_address_type, nullptr, BLE_HS_FOREVER, &parameters, gap_event, nullptr);
}

void on_sync() {
    if (ble_hs_util_ensure_addr(0) != 0 ||
        ble_hs_id_infer_auto(0, &s_address_type) != 0) {
        return;
    }
    advertise();
}

void on_reset(int) {
    s_session.disconnect();
    s_owner_connection = kNoConnection;
    hide_all_passkeys();
}

void host_task(void *) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

int gap_event(ble_gap_event *event, void *) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ble_gap_security_initiate(event->connect.conn_handle);
            } else {
                advertise();
            }
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            release_connection(event->disconnect.conn.conn_handle);
            advertise();
            return 0;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            advertise();
            return 0;
        case BLE_GAP_EVENT_ENC_CHANGE:
            hide_passkey(event->enc_change.conn_handle);
            if (!provisioning::link_is_secure(
                    link_security(event->enc_change.conn_handle))) {
                release_connection(event->enc_change.conn_handle);
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
        if (characteristic == 3) {
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
    send_acknowledgement(connection_handle, result);
    if (!s_session.active()) {
        s_owner_connection = kNoConnection;
    }
    return 0;
}

}  // namespace

esp_err_t start(
    SettingsStore *settings_store,
    PasskeyCallback passkey_callback,
    DeviceContextCallback device_context_callback,
    void *callback_context) {
    if (settings_store == nullptr || passkey_callback == nullptr ||
        device_context_callback == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t storage_result = settings_store->initialize();
    if (storage_result != ESP_OK) {
        return storage_result;
    }
#if defined(CONFIG_BT_NIMBLE_NVS_PERSIST) && CONFIG_BT_NIMBLE_NVS_PERSIST
    if (settings_store->persistence() != SettingsPersistence::plaintext_nvs) {
        return ESP_ERR_INVALID_STATE;
    }
#endif

    s_settings_store = settings_store;
    s_passkey_callback = passkey_callback;
    s_device_context_callback = device_context_callback;
    s_callback_context = callback_context;
    s_session.disconnect();
    s_owner_connection = kNoConnection;

    const esp_err_t init_result = nimble_port_init();
    if (init_result != ESP_OK) {
        s_settings_store = nullptr;
        s_passkey_callback = nullptr;
        s_device_context_callback = nullptr;
        s_callback_context = nullptr;
        return init_result;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_sc_only = 1;
    ble_hs_cfg.sm_sec_lvl = 4;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;

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
        nimble_port_deinit();
        s_settings_store = nullptr;
        s_passkey_callback = nullptr;
        s_device_context_callback = nullptr;
        s_callback_context = nullptr;
        return ESP_FAIL;
    }

    ble_store_config_init();
    s_running = true;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

esp_err_t stop() {
    if (!s_running) {
        return ESP_OK;
    }
    ble_gap_adv_stop();
    const int stop_result = nimble_port_stop();
    const esp_err_t deinit_result = nimble_port_deinit();
    s_session.disconnect();
    s_owner_connection = kNoConnection;
    hide_all_passkeys();
    s_settings_store = nullptr;
    s_passkey_callback = nullptr;
    s_device_context_callback = nullptr;
    s_callback_context = nullptr;
    s_running = false;
    return stop_result == 0 ? deinit_result : ESP_FAIL;
}

bool running() { return s_running; }

}  // namespace ble_provisioning
}  // namespace chatesp
