#include "settings_store.hpp"

#include <algorithm>
#include <limits>

#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"

#ifndef CHATESP_DEVELOPMENT_MODE
#define CHATESP_DEVELOPMENT_MODE 0
#endif

namespace chatesp {
namespace {

constexpr char kNamespace[] = "chatesp_cfg";
constexpr char kPacketKey[] = "packet";
constexpr TickType_t kLockTimeout = pdMS_TO_TICKS(1000);
constexpr provisioning::LinkSecurity kTrustedStoredPacket{true, true, true, true};

class LockGuard {
public:
    explicit LockGuard(SemaphoreHandle_t mutex)
        : mutex_(mutex), locked_(xSemaphoreTake(mutex_, kLockTimeout) == pdTRUE) {}
    ~LockGuard() {
        if (locked_) {
            xSemaphoreGive(mutex_);
        }
    }
    [[nodiscard]] bool locked() const { return locked_; }

private:
    SemaphoreHandle_t mutex_;
    bool locked_;
};

[[maybe_unused]] void clear_bytes(void *data, std::size_t size) {
    volatile std::uint8_t *cursor = static_cast<volatile std::uint8_t *>(data);
    for (std::size_t index = 0; index < size; ++index) {
        cursor[index] = 0;
    }
}

[[maybe_unused]] bool constant_time_equal(
    const std::uint8_t *left, const std::uint8_t *right, std::size_t size) {
    std::uint8_t difference = 0;
    for (std::size_t index = 0; index < size; ++index) {
        difference |= static_cast<std::uint8_t>(left[index] ^ right[index]);
    }
    return difference == 0;
}

}  // namespace

SettingsStore::SettingsStore() {
    mutex_ = xSemaphoreCreateMutexStatic(&mutex_storage_);
}

SettingsStore::~SettingsStore() {
    clear();
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
    }
}

esp_err_t SettingsStore::initialize() {
    LockGuard lock(mutex_);
    if (!lock.locked()) {
        return ESP_ERR_TIMEOUT;
    }
    if (initialized_) {
        return persistence_ == SettingsPersistence::unavailable
                   ? ESP_ERR_NOT_SUPPORTED
                   : ESP_OK;
    }
    initialized_ = true;

#if CHATESP_DEVELOPMENT_MODE
    persistence_ = SettingsPersistence::volatile_development;
    return ESP_OK;
#elif defined(CONFIG_NVS_ENCRYPTION) && CONFIG_NVS_ENCRYPTION
    const esp_err_t init_result = nvs_flash_init();
    if (init_result != ESP_OK) {
        return init_result;
    }
    const esp_partition_t *keys = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, nullptr);
    if (keys == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    nvs_sec_cfg_t security{};
    const esp_err_t key_result = nvs_flash_read_security_cfg(keys, &security);
    clear_bytes(&security, sizeof(security));
    if (key_result != ESP_OK) {
        return key_result;
    }
    persistence_ = SettingsPersistence::encrypted_nvs;
    if (!load_encrypted_record()) {
        settings_.clear();
        version_ = {};
    }
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

provisioning::StoredVersion SettingsStore::stored_version() const {
    LockGuard lock(mutex_);
    if (lock.locked()) {
        return version_;
    }
    provisioning::StoredVersion fail_closed;
    fail_closed.present = true;
    fail_closed.revision = std::numeric_limits<std::uint32_t>::max();
    return fail_closed;
}

bool SettingsStore::store(
    const std::uint8_t *packet,
    std::size_t packet_size,
    const provisioning::ValidationResult &validation) {
    if (packet == nullptr || packet_size < provisioning::kHeaderSize ||
        packet_size > provisioning::kMaximumPacketSize ||
        validation.error != provisioning::ValidationError::none ||
        validation.decision != provisioning::ApplyDecision::apply) {
        return false;
    }

    LockGuard lock(mutex_);
    if (!lock.locked()) {
        return false;
    }
    if (version_.present && validation.revision <= version_.revision) {
        return false;
    }

    provisioning::SettingsRecord next;
    if (!next.assign(validation)) {
        return false;
    }
    if (persistence_ == SettingsPersistence::unavailable) {
        next.clear();
        return false;
    }
    if (persistence_ == SettingsPersistence::encrypted_nvs &&
        !store_encrypted_packet(packet, packet_size)) {
        next.clear();
        return false;
    }

    settings_.clear();
    settings_ = next;
    next.clear();
    version_.present = true;
    version_.revision = validation.revision;
    version_.fingerprint = validation.fingerprint;
    return true;
}

bool SettingsStore::read(provisioning::SettingsRecord *settings) const {
    if (settings == nullptr) {
        return false;
    }
    LockGuard lock(mutex_);
    if (!lock.locked() || !version_.present) {
        return false;
    }
    settings->clear();
    *settings = settings_;
    return true;
}

SettingsPersistence SettingsStore::persistence() const {
    LockGuard lock(mutex_);
    return lock.locked() ? persistence_ : SettingsPersistence::unavailable;
}

bool SettingsStore::is_volatile() const {
    return persistence() == SettingsPersistence::volatile_development;
}

bool SettingsStore::load_encrypted_record() {
#if defined(CONFIG_NVS_ENCRYPTION) && CONFIG_NVS_ENCRYPTION
    nvs_handle_t handle = 0;
    const esp_err_t open_result = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (open_result != ESP_OK) {
        return open_result == ESP_ERR_NVS_NOT_FOUND;
    }

    std::array<std::uint8_t, provisioning::kMaximumPacketSize> packet{};
    std::size_t packet_size = packet.size();
    const esp_err_t read_result = nvs_get_blob(
        handle, kPacketKey, packet.data(), &packet_size);
    nvs_close(handle);
    if (read_result == ESP_ERR_NVS_NOT_FOUND) {
        clear_bytes(packet.data(), packet.size());
        return true;
    }
    if (read_result != ESP_OK || packet_size < provisioning::kHeaderSize ||
        packet_size > packet.size()) {
        clear_bytes(packet.data(), packet.size());
        return false;
    }

    const provisioning::ValidationResult validation =
        provisioning::validate_settings_packet(
            packet.data(), packet_size, kTrustedStoredPacket, {});
    const bool valid = validation.error == provisioning::ValidationError::none &&
        validation.decision == provisioning::ApplyDecision::apply &&
        settings_.assign(validation);
    if (valid) {
        version_.present = true;
        version_.revision = validation.revision;
        version_.fingerprint = validation.fingerprint;
    }
    clear_bytes(packet.data(), packet.size());
    return valid;
#else
    return false;
#endif
}

bool SettingsStore::store_encrypted_packet(
    const std::uint8_t *packet, std::size_t packet_size) {
#if defined(CONFIG_NVS_ENCRYPTION) && CONFIG_NVS_ENCRYPTION
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t result = nvs_set_blob(handle, kPacketKey, packet, packet_size);
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }

    std::array<std::uint8_t, provisioning::kMaximumPacketSize> verification{};
    std::size_t verification_size = verification.size();
    if (result == ESP_OK) {
        result = nvs_get_blob(
            handle, kPacketKey, verification.data(), &verification_size);
    }
    nvs_close(handle);
    const bool verified = result == ESP_OK && verification_size == packet_size &&
        constant_time_equal(packet, verification.data(), packet_size);
    clear_bytes(verification.data(), verification.size());
    return verified;
#else
    (void)packet;
    (void)packet_size;
    return false;
#endif
}

void SettingsStore::clear() {
    if (mutex_ == nullptr) {
        return;
    }
    LockGuard lock(mutex_);
    if (!lock.locked()) {
        return;
    }
    settings_.clear();
    version_ = {};
}

}  // namespace chatesp
