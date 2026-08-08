#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "chatesp/provisioning_packet.hpp"

namespace chatesp {
namespace provisioning {

constexpr std::size_t kControlFrameSize = 16;
constexpr std::size_t kDataFrameHeaderSize = 14;
constexpr std::size_t kMaximumFrameDataSize = 180;
constexpr std::size_t kAcknowledgementSize = 44;
constexpr std::uint16_t kAcknowledgementActiveVersionFlag = 0x0001;

enum class TransferError : std::uint8_t {
    none,
    authentication_required,
    bad_length,
    bad_magic,
    unsupported_version,
    bad_operation,
    bad_flags,
    bad_packet_size,
    bad_frame_size,
    no_transfer,
    wrong_transfer_id,
    wrong_offset,
    excess_data,
};

enum class AcknowledgementStatus : std::uint8_t {
    applied = 0x00,
    unchanged = 0x01,
    authentication_required = 0x10,
    unsupported_version = 0x11,
    malformed_transfer = 0x12,
    malformed_packet = 0x13,
    invalid_field = 0x14,
    stale_revision = 0x15,
    revision_conflict = 0x16,
    storage_failure = 0x17,
    busy = 0x18,
};

class TransferAssembler {
public:
    [[nodiscard]] TransferError handle_control(
        const std::uint8_t *frame,
        std::size_t frame_size,
        const LinkSecurity &security);

    [[nodiscard]] TransferError handle_data(
        const std::uint8_t *frame,
        std::size_t frame_size,
        const LinkSecurity &security);

    void reset();

    [[nodiscard]] bool active() const;
    [[nodiscard]] bool complete() const;
    [[nodiscard]] std::uint32_t transfer_id() const;
    [[nodiscard]] std::size_t received_size() const;
    [[nodiscard]] std::size_t packet_size() const;
    [[nodiscard]] const std::uint8_t *packet_data() const;
    [[nodiscard]] std::uint8_t version() const;

private:
    std::array<std::uint8_t, kMaximumPacketSize> packet_{};
    std::uint32_t transfer_id_ = 0;
    std::size_t expected_size_ = 0;
    std::size_t received_size_ = 0;
    std::size_t maximum_frame_data_size_ = 0;
    std::uint8_t version_ = kProtocolVersion;
    bool active_ = false;
};

[[nodiscard]] std::array<std::uint8_t, kAcknowledgementSize>
make_acknowledgement(
    AcknowledgementStatus status,
    std::uint32_t revision,
    const std::array<std::uint8_t, kFingerprintSize> &fingerprint,
    std::uint8_t version = kProtocolVersion,
    std::uint16_t flags = 0);

}  // namespace provisioning
}  // namespace chatesp
