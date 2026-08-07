#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace chatesp {

class AudioPlayback {
public:
    static constexpr std::uint32_t kSampleRateHz = 24'000;
    static constexpr std::uint8_t kBitsPerSample = 16;
    static constexpr std::uint8_t kChannelCount = 1;
    static constexpr std::size_t kChunkSamples = kSampleRateHz / 50;

    AudioPlayback();
    ~AudioPlayback();

    AudioPlayback(const AudioPlayback &) = delete;
    AudioPlayback &operator=(const AudioPlayback &) = delete;

    esp_err_t start(int volume_percent = 70);
    esp_err_t play(const std::int16_t *samples, std::size_t sample_count);
    esp_err_t stop();
    void cancel();

    [[nodiscard]] bool active() const;
    [[nodiscard]] bool cancelled() const;

private:
    void release_session();

    void *codec_ = nullptr;
    std::atomic<bool> cancelled_{false};
    bool active_ = false;
    bool session_acquired_ = false;
};

}  // namespace chatesp
