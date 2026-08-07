#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "chatesp/audio_buffer.hpp"
#include "esp_err.h"

namespace chatesp {

class AudioCapture {
public:
    static constexpr std::uint32_t kSampleRateHz = 16'000;
    static constexpr std::uint8_t kBitsPerSample = 16;
    static constexpr std::uint8_t kChannelCount = 1;
    static constexpr std::uint32_t kMaximumSeconds = 30;
    static constexpr std::size_t kMaximumSamples =
        kSampleRateHz * kMaximumSeconds;
    static constexpr std::size_t kChunkSamples = kSampleRateHz / 50;

    AudioCapture();
    ~AudioCapture();

    AudioCapture(const AudioCapture &) = delete;
    AudioCapture &operator=(const AudioCapture &) = delete;

    esp_err_t start();
    esp_err_t capture_chunk();
    esp_err_t stop();
    void cancel();
    void discard();

    [[nodiscard]] const std::int16_t *samples() const;
    [[nodiscard]] std::size_t sample_count() const;
    [[nodiscard]] bool active() const;
    [[nodiscard]] bool cancelled() const;

private:
    void release_buffer();
    void release_session();

    void *codec_ = nullptr;
    std::int16_t *samples_ = nullptr;
    AudioSampleBudget budget_{kMaximumSamples};
    std::atomic<bool> cancelled_{false};
    bool active_ = false;
    bool session_acquired_ = false;
};

}  // namespace chatesp
