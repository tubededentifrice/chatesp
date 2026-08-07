#include "chatesp/pcm16_stream.hpp"

namespace chatesp {
namespace runtime {
namespace {

std::int16_t decode_little_endian(
    std::uint8_t low, std::uint8_t high) {
    const std::uint16_t value = static_cast<std::uint16_t>(low) |
                                (static_cast<std::uint16_t>(high) << 8U);
    const std::int32_t signed_value =
        value <= 0x7FFFU ? static_cast<std::int32_t>(value)
                         : static_cast<std::int32_t>(value) - 0x10000;
    return static_cast<std::int16_t>(signed_value);
}

}  // namespace

Pcm16Stream::Status Pcm16Stream::write(
    const std::uint8_t *data, std::size_t size, Output output,
    void *context) {
    if (output_failed_) {
        return Status::output_failed;
    }
    if (output == nullptr || (size != 0 && data == nullptr)) {
        return Status::invalid_argument;
    }

    std::size_t offset = 0;
    while (offset < size) {
        std::size_t sample_count = 0;
        if (has_carry_) {
            samples_[sample_count++] =
                decode_little_endian(carry_, data[offset++]);
            has_carry_ = false;
        }
        while (sample_count < samples_.size() && size - offset >= 2) {
            samples_[sample_count++] =
                decode_little_endian(data[offset], data[offset + 1]);
            offset += 2;
        }
        if (sample_count != 0 &&
            !output(context, samples_.data(), sample_count)) {
            output_failed_ = true;
            return Status::output_failed;
        }
        if (size - offset == 1) {
            carry_ = data[offset];
            has_carry_ = true;
            ++offset;
        }
    }
    return Status::none;
}

Pcm16Stream::Status Pcm16Stream::finish() const {
    if (output_failed_) {
        return Status::output_failed;
    }
    return has_carry_ ? Status::incomplete_sample : Status::none;
}

void Pcm16Stream::reset() {
    carry_ = 0;
    has_carry_ = false;
    output_failed_ = false;
}

}  // namespace runtime
}  // namespace chatesp
