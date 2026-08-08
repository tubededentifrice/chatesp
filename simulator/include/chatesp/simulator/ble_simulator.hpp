#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "chatesp/ble_settings.hpp"
#include "chatesp/provisioning_session.hpp"

namespace chatesp::simulator {

constexpr std::size_t kMaximumBleFuzzCases = 100'000;

enum class BleState : std::uint8_t {
    off,
    advertising,
    pairing,
    connected,
};

enum class BleFault : std::uint8_t {
    none,
    disconnect_after_control,
    disconnect_after_data,
    drop_acknowledgement,
    corrupt_data,
    storage_failure,
};

enum class BleOutcome : std::uint8_t {
    none,
    applied,
    unchanged,
    authentication_required,
    unsupported_version,
    malformed_transfer,
    malformed_packet,
    invalid_field,
    stale_revision,
    revision_conflict,
    storage_failure,
    busy,
    disconnected,
    pairing_failed,
    protocol_error,
};

struct BleSnapshot {
    BleState state = BleState::off;
    BleOutcome outcome = BleOutcome::none;
    std::uint32_t passkey = 0;
    std::uint32_t active_revision = 0;
    std::size_t attempts = 0;
    std::size_t storage_writes = 0;
    std::size_t fuzz_cases = 0;
    bool secure = false;
    bool bonded = false;
    bool passkey_visible = false;
};

class SimulatorSettingsSink final : public provisioning::SettingsSink {
public:
    [[nodiscard]] provisioning::StoredVersion stored_version() const override;
    [[nodiscard]] bool store(
        const std::uint8_t *packet,
        std::size_t packet_size,
        const provisioning::ValidationResult &validation) override;

    void fail_next_store();
    void clear();
    [[nodiscard]] std::uint32_t revision() const;
    [[nodiscard]] std::size_t writes() const;

private:
    provisioning::SettingsRecord record_;
    std::size_t writes_ = 0;
    bool fail_next_store_ = false;
};

class BleSimulator {
public:
    explicit BleSimulator(bool development_mode = false);

    void factory_reset();
    void start_radio();
    void stop_radio();
    void restart_radio();
    void reboot();
    bool connect();
    bool confirm_pairing(std::uint32_t passkey);
    bool reject_pairing();
    bool disconnect();
    bool provision(std::uint32_t revision, BleFault fault = BleFault::none);
    bool fuzz(std::size_t cases, std::uint32_t seed);

    [[nodiscard]] BleSnapshot snapshot() const;

private:
    void disconnect_link();
    void set_outcome_from_acknowledgement(
        const provisioning::SessionResult &result);

    const bool development_mode_;
    provisioning::ProvisioningSession session_;
    SimulatorSettingsSink settings_;
    provisioning::LinkSecurity security_{};
    BleState state_ = BleState::off;
    BleOutcome outcome_ = BleOutcome::none;
    std::uint32_t passkey_ = 0;
    std::uint32_t next_passkey_ = 123'456;
    std::size_t attempts_ = 0;
    std::size_t fuzz_cases_ = 0;
    bool bonded_ = false;
};

[[nodiscard]] const char *ble_state_name(BleState state);
[[nodiscard]] const char *ble_outcome_name(BleOutcome outcome);
[[nodiscard]] bool parse_ble_fault(std::string_view text, BleFault &fault);

}  // namespace chatesp::simulator
