#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "chatesp/provisioning_packet.hpp"
#include "chatesp/provisioning_transfer.hpp"

namespace chatesp {
namespace provisioning {

class SettingsSink {
public:
    virtual ~SettingsSink() = default;

    [[nodiscard]] virtual StoredVersion stored_version() const = 0;
    [[nodiscard]] virtual bool store(
        const std::uint8_t *packet,
        std::size_t packet_size,
        const ValidationResult &validation) = 0;
};

struct SessionResult {
    bool has_acknowledgement = false;
    std::array<std::uint8_t, kAcknowledgementSize> acknowledgement{};
};

class ProvisioningSession {
public:
    ~ProvisioningSession() { disconnect(); }

    [[nodiscard]] SessionResult handle_control(
        const std::uint8_t *frame,
        std::size_t frame_size,
        const LinkSecurity &security);

    [[nodiscard]] SessionResult handle_data(
        const std::uint8_t *frame,
        std::size_t frame_size,
        const LinkSecurity &security,
        SettingsSink &settings);

    [[nodiscard]] SessionResult busy_acknowledgement() const;
    void disconnect();

    [[nodiscard]] bool active() const;

private:
    [[nodiscard]] SessionResult transfer_error(TransferError error);

    TransferAssembler assembler_;
};

}  // namespace provisioning
}  // namespace chatesp
