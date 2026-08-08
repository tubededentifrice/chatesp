#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "chatesp/ble_settings.hpp"
#include "chatesp/provisioning_session.hpp"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace chatesp {

enum class SettingsPersistence : std::uint8_t {
    unavailable,
    volatile_development,
    plaintext_nvs,
};

class SettingsStore final : public provisioning::SettingsSink {
public:
    SettingsStore();
    ~SettingsStore() override;

    SettingsStore(const SettingsStore &) = delete;
    SettingsStore &operator=(const SettingsStore &) = delete;

    esp_err_t initialize();

    [[nodiscard]] provisioning::StoredVersion stored_version() const override;
    [[nodiscard]] bool store(
        const std::uint8_t *packet,
        std::size_t packet_size,
        const provisioning::ValidationResult &validation) override;

    [[nodiscard]] bool read(provisioning::SettingsRecord *settings) const;
    [[nodiscard]] SettingsPersistence persistence() const;
    [[nodiscard]] bool is_volatile() const;

private:
    [[nodiscard]] bool load_persistent_record();
    [[nodiscard]] bool store_persistent_packet(
        const std::uint8_t *packet, std::size_t packet_size);
    void clear();

    mutable StaticSemaphore_t mutex_storage_{};
    mutable SemaphoreHandle_t mutex_ = nullptr;
    provisioning::SettingsRecord settings_{};
    provisioning::StoredVersion version_{};
    SettingsPersistence persistence_ = SettingsPersistence::unavailable;
    bool initialized_ = false;
};

}  // namespace chatesp
