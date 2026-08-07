#include "chatesp/provider_helpers.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace chatesp {
namespace provider {
namespace {

constexpr std::uint32_t kWavSampleRateHz = 16'000;
constexpr std::uint16_t kWavChannels = 1;
constexpr std::uint16_t kWavBitsPerSample = 16;

void put_u16(std::uint8_t *output, std::uint16_t value) {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
}

void put_u32(std::uint8_t *output, std::uint32_t value) {
    output[0] = static_cast<std::uint8_t>(value);
    output[1] = static_cast<std::uint8_t>(value >> 8U);
    output[2] = static_cast<std::uint8_t>(value >> 16U);
    output[3] = static_cast<std::uint8_t>(value >> 24U);
}

bool valid_byte_sequence(const char *data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return false;
    }
    for (std::size_t index = 0; index < size; ++index) {
        const auto byte = static_cast<unsigned char>(data[index]);
        if (byte < 0x21 || byte > 0x7e) {
            return false;
        }
    }
    return true;
}

bool valid_path(const char *path) {
    if (path == nullptr || path[0] != '/' || path[1] == '\0') {
        return false;
    }
    const char *segment = path + 1;
    bool in_query = false;
    for (const char *cursor = path + 1; *cursor != '\0'; ++cursor) {
        const auto byte = static_cast<unsigned char>(*cursor);
        if (byte < 0x21 || byte > 0x7e || *cursor == '#' ||
            *cursor == '\\') {
            return false;
        }
        if (!in_query && (*cursor == '/' || *cursor == '?')) {
            const std::size_t segment_size =
                static_cast<std::size_t>(cursor - segment);
            if ((segment_size == 1 && segment[0] == '.') ||
                (segment_size == 2 && segment[0] == '.' &&
                 segment[1] == '.')) {
                return false;
            }
            if (*cursor == '?') {
                in_query = true;
            } else {
                segment = cursor + 1;
            }
        } else if (in_query && *cursor == '?') {
            return false;
        }
    }
    if (in_query) {
        return true;
    }
    const char *end = path + std::strlen(path);
    const std::size_t segment_size = static_cast<std::size_t>(end - segment);
    return !((segment_size == 1 && segment[0] == '.') ||
             (segment_size == 2 && segment[0] == '.' && segment[1] == '.'));
}

void copy_segment(
    const std::uint8_t *source, std::size_t source_size,
    std::size_t segment_start, std::size_t absolute_offset,
    std::uint8_t *output, std::size_t capacity, std::size_t &written) {
    if (absolute_offset >= segment_start + source_size ||
        absolute_offset < segment_start || written >= capacity) {
        return;
    }
    const std::size_t source_offset = absolute_offset - segment_start;
    const std::size_t count = std::min(
        source_size - source_offset, capacity - written);
    if (count != 0) {
        std::memcpy(output + written, source + source_offset, count);
        written += count;
    }
}

}  // namespace

bool valid_secret(SecretView secret, std::size_t max_size) {
    return secret.size <= max_size && valid_byte_sequence(secret.data, secret.size);
}

agent::Error build_bearer_header(
    SecretView secret, char *output, std::size_t capacity) {
    constexpr char prefix[] = "Bearer ";
    if (!valid_secret(secret, 512) || output == nullptr || capacity == 0 ||
        secret.size > capacity - 1 || sizeof(prefix) - 1 > capacity - 1 - secret.size) {
        return agent::Error::invalid_argument;
    }
    std::memcpy(output, prefix, sizeof(prefix) - 1);
    std::memcpy(output + sizeof(prefix) - 1, secret.data, secret.size);
    output[sizeof(prefix) - 1 + secret.size] = '\0';
    return agent::Error::none;
}

agent::Error build_api_url(
    const char *base, std::size_t base_size, const char *path,
    agent::FixedText<agent::Limits::max_url_bytes> &url) {
    url.clear();
    constexpr char scheme[] = "https://";
    if (base == nullptr || base_size <= sizeof(scheme) - 1 ||
        base_size > agent::Limits::max_url_bytes ||
        std::memcmp(base, scheme, sizeof(scheme) - 1) != 0 ||
        !valid_path(path)) {
        return agent::Error::invalid_argument;
    }
    const char *authority = base + sizeof(scheme) - 1;
    const char *authority_end = static_cast<const char *>(std::memchr(
        authority, '/', base_size - (sizeof(scheme) - 1)));
    if (authority_end == nullptr) {
        authority_end = base + base_size;
    }
    if (authority == authority_end ||
        std::memchr(authority, '@', authority_end - authority) != nullptr) {
        return agent::Error::invalid_argument;
    }
    for (std::size_t index = 0; index < base_size; ++index) {
        const auto byte = static_cast<unsigned char>(base[index]);
        if (byte < 0x21 || byte > 0x7e || base[index] == '?' ||
            base[index] == '#' || base[index] == '\\') {
            return agent::Error::invalid_argument;
        }
    }
    while (base_size != 0 && base[base_size - 1] == '/') {
        --base_size;
    }
    if (base_size <= sizeof(scheme) - 1 || !url.append(base, base_size) ||
        !url.append(path)) {
        url.clear();
        return agent::Error::limit_exceeded;
    }
    return agent::Error::none;
}

agent::Error WavPcmStream::configure(
    const std::uint8_t *prefix, std::size_t prefix_size,
    const agent::AudioView &audio, const std::uint8_t *suffix,
    std::size_t suffix_size) {
    configured_ = false;
    total_size_ = 0;
    offset_ = 0;
    if ((prefix_size != 0 && prefix == nullptr) ||
        (suffix_size != 0 && suffix == nullptr) || audio.data == nullptr ||
        audio.size == 0 || audio.size > agent::Limits::max_recording_pcm_bytes ||
        audio.size % 2 != 0 || audio.sample_rate_hz != kWavSampleRateHz ||
        audio.channels != kWavChannels ||
        audio.bits_per_sample != kWavBitsPerSample ||
        audio.size > std::numeric_limits<std::uint32_t>::max() - 36U ||
        prefix_size > std::numeric_limits<std::size_t>::max() - 44U ||
        audio.size > std::numeric_limits<std::size_t>::max() - prefix_size - 44U ||
        suffix_size >
            std::numeric_limits<std::size_t>::max() - prefix_size - 44U - audio.size) {
        return agent::Error::invalid_argument;
    }

    prefix_ = prefix;
    prefix_size_ = prefix_size;
    pcm_ = audio.data;
    pcm_size_ = audio.size;
    suffix_ = suffix;
    suffix_size_ = suffix_size;
    total_size_ = prefix_size_ + wav_header_.size() + pcm_size_ + suffix_size_;
    wav_header_.fill(0);
    std::memcpy(wav_header_.data(), "RIFF", 4);
    put_u32(wav_header_.data() + 4, static_cast<std::uint32_t>(36U + pcm_size_));
    std::memcpy(wav_header_.data() + 8, "WAVEfmt ", 8);
    put_u32(wav_header_.data() + 16, 16);
    put_u16(wav_header_.data() + 20, 1);
    put_u16(wav_header_.data() + 22, kWavChannels);
    put_u32(wav_header_.data() + 24, kWavSampleRateHz);
    put_u32(
        wav_header_.data() + 28,
        kWavSampleRateHz * kWavChannels * kWavBitsPerSample / 8U);
    put_u16(
        wav_header_.data() + 32,
        kWavChannels * kWavBitsPerSample / 8U);
    put_u16(wav_header_.data() + 34, kWavBitsPerSample);
    std::memcpy(wav_header_.data() + 36, "data", 4);
    put_u32(wav_header_.data() + 40, static_cast<std::uint32_t>(pcm_size_));
    configured_ = true;
    return agent::Error::none;
}

void WavPcmStream::reset() { offset_ = 0; }

agent::Error WavPcmStream::read(
    std::uint8_t *output, std::size_t capacity, std::size_t &read_size) {
    read_size = 0;
    if (!configured_ || output == nullptr || capacity == 0 ||
        offset_ > total_size_) {
        return agent::Error::invalid_argument;
    }
    const std::size_t requested = std::min(capacity, total_size_ - offset_);
    while (read_size < requested) {
        const std::size_t absolute = offset_ + read_size;
        const std::size_t before = read_size;
        copy_segment(
            prefix_, prefix_size_, 0, absolute, output, requested, read_size);
        copy_segment(
            wav_header_.data(), wav_header_.size(), prefix_size_, absolute,
            output, requested, read_size);
        copy_segment(
            pcm_, pcm_size_, prefix_size_ + wav_header_.size(), absolute,
            output, requested, read_size);
        copy_segment(
            suffix_, suffix_size_,
            prefix_size_ + wav_header_.size() + pcm_size_, absolute,
            output, requested, read_size);
        if (read_size == before) {
            return agent::Error::invalid_argument;
        }
    }
    offset_ += read_size;
    return agent::Error::none;
}

void BoundedResponseBuffer::reset() {
    size_ = 0;
    if (storage_ != nullptr) {
        storage_[0] = '\0';
    }
}

bool BoundedResponseBuffer::append(
    const std::uint8_t *data, std::size_t size) {
    if ((size != 0 && data == nullptr) || storage_ == nullptr ||
        size > capacity_ - size_) {
        return false;
    }
    if (size != 0) {
        std::memcpy(storage_ + size_, data, size);
        size_ += size;
    }
    storage_[size_] = '\0';
    return true;
}

}  // namespace provider
}  // namespace chatesp
