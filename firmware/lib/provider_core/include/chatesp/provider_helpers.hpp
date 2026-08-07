#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "chatesp/agent_types.hpp"

namespace chatesp {
namespace provider {

struct SecretView {
    const char *data = nullptr;
    std::size_t size = 0;
};

[[nodiscard]] bool valid_secret(SecretView secret, std::size_t max_size);

agent::Error build_bearer_header(
    SecretView secret, char *output, std::size_t capacity);

agent::Error build_api_url(
    const char *base, std::size_t base_size, const char *path,
    agent::FixedText<agent::Limits::max_url_bytes> &url);

class WavPcmStream {
public:
    agent::Error configure(
        const std::uint8_t *prefix, std::size_t prefix_size,
        const agent::AudioView &audio, const std::uint8_t *suffix,
        std::size_t suffix_size);
    void reset();
    agent::Error read(
        std::uint8_t *output, std::size_t capacity, std::size_t &read_size);

    [[nodiscard]] std::size_t size() const { return total_size_; }
    [[nodiscard]] const std::array<std::uint8_t, 44> &wav_header() const {
        return wav_header_;
    }

private:
    const std::uint8_t *prefix_ = nullptr;
    std::size_t prefix_size_ = 0;
    const std::uint8_t *pcm_ = nullptr;
    std::size_t pcm_size_ = 0;
    const std::uint8_t *suffix_ = nullptr;
    std::size_t suffix_size_ = 0;
    std::array<std::uint8_t, 44> wav_header_{};
    std::size_t total_size_ = 0;
    std::size_t offset_ = 0;
    bool configured_ = false;
};

class BoundedResponseBuffer {
public:
    BoundedResponseBuffer(char *storage, std::size_t capacity)
        : storage_(storage), capacity_(capacity) {
        reset();
    }

    void reset();
    [[nodiscard]] bool append(const std::uint8_t *data, std::size_t size);

    [[nodiscard]] const char *data() const {
        return storage_ == nullptr ? "" : storage_;
    }
    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] std::size_t capacity() const { return capacity_; }

private:
    char *storage_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
};

}  // namespace provider
}  // namespace chatesp
