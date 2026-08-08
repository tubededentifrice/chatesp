#include "chatesp/provisioning_transfer.hpp"

#include <algorithm>

namespace chatesp {
namespace provisioning {
namespace {

constexpr std::array<std::uint8_t, 4> kControlMagic{{'C', 'E', 'S', 'B'}};
constexpr std::array<std::uint8_t, 4> kDataMagic{{'C', 'E', 'S', 'D'}};

std::uint16_t read_u16(const std::uint8_t *data) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0]) << 8U) |
        static_cast<std::uint16_t>(data[1]));
}

std::uint32_t read_u32(const std::uint8_t *data) {
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
        (static_cast<std::uint32_t>(data[1]) << 16U) |
        (static_cast<std::uint32_t>(data[2]) << 8U) |
        static_cast<std::uint32_t>(data[3]);
}

void write_u16(std::uint8_t *data, std::uint16_t value) {
    data[0] = static_cast<std::uint8_t>(value >> 8U);
    data[1] = static_cast<std::uint8_t>(value);
}

void write_u32(std::uint8_t *data, std::uint32_t value) {
    data[0] = static_cast<std::uint8_t>(value >> 24U);
    data[1] = static_cast<std::uint8_t>(value >> 16U);
    data[2] = static_cast<std::uint8_t>(value >> 8U);
    data[3] = static_cast<std::uint8_t>(value);
}

}  // namespace

TransferError TransferAssembler::handle_control(
    const std::uint8_t *frame,
    std::size_t frame_size,
    const LinkSecurity &security) {
    if (!link_is_secure(security)) {
        reset();
        return TransferError::authentication_required;
    }
    if (frame == nullptr || frame_size != kControlFrameSize) {
        return TransferError::bad_length;
    }
    if (!std::equal(kControlMagic.begin(), kControlMagic.end(), frame)) {
        return TransferError::bad_magic;
    }
    if (!supported_protocol_version(frame[4])) {
        return TransferError::unsupported_version;
    }
    version_ = frame[4];
    if (frame[6] != 0 || frame[7] != 0) {
        return TransferError::bad_flags;
    }

    const std::uint8_t operation = frame[5];
    const std::size_t packet_size = read_u16(frame + 12);
    const std::size_t frame_data_size = read_u16(frame + 14);
    if (operation == 2) {
        if (packet_size != 0 || frame_data_size != 0) {
            return TransferError::bad_packet_size;
        }
        reset();
        return TransferError::none;
    }
    if (operation != 1) {
        return TransferError::bad_operation;
    }
    if (packet_size < kHeaderSize || packet_size > kMaximumPacketSize) {
        return TransferError::bad_packet_size;
    }
    if (frame_data_size == 0 || frame_data_size > kMaximumFrameDataSize) {
        return TransferError::bad_frame_size;
    }

    transfer_id_ = read_u32(frame + 8);
    expected_size_ = packet_size;
    received_size_ = 0;
    maximum_frame_data_size_ = frame_data_size;
    active_ = true;
    return TransferError::none;
}

TransferError TransferAssembler::handle_data(
    const std::uint8_t *frame,
    std::size_t frame_size,
    const LinkSecurity &security) {
    if (!link_is_secure(security)) {
        reset();
        return TransferError::authentication_required;
    }
    if (frame == nullptr || frame_size < kDataFrameHeaderSize + 1) {
        return TransferError::bad_length;
    }
    if (!std::equal(kDataMagic.begin(), kDataMagic.end(), frame)) {
        return TransferError::bad_magic;
    }
    if (!supported_protocol_version(frame[4]) || frame[4] != version_) {
        return TransferError::unsupported_version;
    }
    if (frame[5] != 0) {
        return TransferError::bad_flags;
    }
    if (!active_ || received_size_ == expected_size_) {
        return TransferError::no_transfer;
    }
    if (read_u32(frame + 6) != transfer_id_) {
        return TransferError::wrong_transfer_id;
    }
    if (read_u16(frame + 10) != received_size_) {
        return TransferError::wrong_offset;
    }
    const std::size_t data_size = read_u16(frame + 12);
    if (data_size == 0 || data_size > maximum_frame_data_size_ ||
        frame_size != kDataFrameHeaderSize + data_size) {
        return TransferError::bad_frame_size;
    }
    if (data_size > expected_size_ - received_size_) {
        return TransferError::excess_data;
    }
    std::copy(
        frame + kDataFrameHeaderSize,
        frame + kDataFrameHeaderSize + data_size,
        packet_.begin() + received_size_);
    received_size_ += data_size;
    return TransferError::none;
}

void TransferAssembler::reset() {
    std::fill(packet_.begin(), packet_.end(), 0);
    transfer_id_ = 0;
    expected_size_ = 0;
    received_size_ = 0;
    maximum_frame_data_size_ = 0;
    version_ = kProtocolVersion;
    active_ = false;
}

bool TransferAssembler::active() const { return active_; }
bool TransferAssembler::complete() const {
    return active_ && received_size_ == expected_size_;
}
std::uint32_t TransferAssembler::transfer_id() const { return transfer_id_; }
std::size_t TransferAssembler::received_size() const { return received_size_; }
std::size_t TransferAssembler::packet_size() const { return expected_size_; }
const std::uint8_t *TransferAssembler::packet_data() const { return packet_.data(); }
std::uint8_t TransferAssembler::version() const { return version_; }

std::array<std::uint8_t, kAcknowledgementSize> make_acknowledgement(
    AcknowledgementStatus status,
    std::uint32_t revision,
    const std::array<std::uint8_t, kFingerprintSize> &fingerprint,
    std::uint8_t version,
    std::uint16_t flags) {
    std::array<std::uint8_t, kAcknowledgementSize> result{};
    result[0] = 'C';
    result[1] = 'E';
    result[2] = 'S';
    result[3] = 'A';
    result[4] = supported_protocol_version(version) ? version : kProtocolVersion;
    result[5] = static_cast<std::uint8_t>(status);
    write_u16(result.data() + 6, flags);
    write_u32(result.data() + 8, revision);
    std::copy(fingerprint.begin(), fingerprint.end(), result.begin() + 12);
    return result;
}

}  // namespace provisioning
}  // namespace chatesp
