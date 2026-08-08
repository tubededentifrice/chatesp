#include "audio_playback.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "bsp/esp-bsp.h"
#include "chatesp/audio_session.hpp"
#include "esp_codec_dev.h"

namespace chatesp {
namespace {

esp_codec_dev_handle_t codec_handle(void *codec) {
    return static_cast<esp_codec_dev_handle_t>(codec);
}

}  // namespace

AudioPlayback::AudioPlayback() = default;

AudioPlayback::~AudioPlayback() {
    cancel();
    stop();
    if (codec_ != nullptr) {
        esp_codec_dev_delete(codec_handle(codec_));
        codec_ = nullptr;
    }
}

esp_err_t AudioPlayback::start(int volume_percent) {
    if (active_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (volume_percent < 0 || volume_percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    cancelled_.store(false, std::memory_order_release);

    if (!shared_audio_session_gate().try_acquire(AudioSession::playback)) {
        return ESP_ERR_INVALID_STATE;
    }
    session_acquired_ = true;

    if (codec_ == nullptr) {
        codec_ = bsp_audio_codec_speaker_init();
        if (codec_ == nullptr) {
            release_session();
            return ESP_FAIL;
        }
    }

    const int volume_result =
        esp_codec_dev_set_out_vol(codec_handle(codec_), volume_percent);
    if (volume_result != ESP_CODEC_DEV_OK) {
        release_session();
        return static_cast<esp_err_t>(volume_result);
    }
    requested_volume_percent_.store(
        volume_percent, std::memory_order_release);
    applied_volume_percent_ = volume_percent;

    esp_codec_dev_sample_info_t format = {
        .bits_per_sample = kBitsPerSample,
        .channel = kChannelCount,
        .channel_mask = 0,
        .sample_rate = kSampleRateHz,
        .mclk_multiple = 0,
    };
    const int open_result = esp_codec_dev_open(codec_handle(codec_), &format);
    if (open_result != ESP_CODEC_DEV_OK) {
        esp_codec_dev_close(codec_handle(codec_));
        release_session();
        return static_cast<esp_err_t>(open_result);
    }

    active_ = true;
    return ESP_OK;
}

esp_err_t AudioPlayback::play(
    const std::int16_t *samples, std::size_t sample_count) {
    if (!active_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_count == 0) {
        return ESP_OK;
    }
    if (samples == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    std::array<std::int16_t, kChunkSamples> scratch{};
    std::size_t offset = 0;
    while (offset < sample_count) {
        if (cancelled_.load(std::memory_order_acquire)) {
            return ESP_ERR_INVALID_STATE;
        }
        const std::size_t chunk_samples =
            std::min(kChunkSamples, sample_count - offset);
        const int requested_volume = requested_volume_percent_.load(
            std::memory_order_acquire);
        if (requested_volume != applied_volume_percent_) {
            (void)esp_codec_dev_set_out_vol(
                codec_handle(codec_), requested_volume);
            // One failed optional update must not retry for each PCM chunk or
            // stop speech. A later user change gets one new attempt.
            applied_volume_percent_ = requested_volume;
        }
        std::memcpy(
            scratch.data(), samples + offset,
            chunk_samples * sizeof(std::int16_t));
        const int write_result = esp_codec_dev_write(
            codec_handle(codec_), scratch.data(),
            static_cast<int>(chunk_samples * sizeof(std::int16_t)));
        if (write_result != ESP_CODEC_DEV_OK) {
            return static_cast<esp_err_t>(write_result);
        }
        offset += chunk_samples;
    }
    return cancelled_.load(std::memory_order_acquire)
               ? ESP_ERR_INVALID_STATE
               : ESP_OK;
}

esp_err_t AudioPlayback::set_volume(int volume_percent) {
    if (volume_percent < 0 || volume_percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    requested_volume_percent_.store(
        volume_percent, std::memory_order_release);
    return ESP_OK;
}

esp_err_t AudioPlayback::stop() {
    esp_err_t result = ESP_OK;
    if (active_) {
        const int close_result = esp_codec_dev_close(codec_handle(codec_));
        if (close_result != ESP_CODEC_DEV_OK) {
            result = static_cast<esp_err_t>(close_result);
        }
        active_ = false;
    }
    applied_volume_percent_ = -1;
    release_session();
    return result;
}

void AudioPlayback::cancel() {
    cancelled_.store(true, std::memory_order_release);
}

bool AudioPlayback::active() const { return active_; }

bool AudioPlayback::cancelled() const {
    return cancelled_.load(std::memory_order_acquire);
}

void AudioPlayback::release_session() {
    if (!session_acquired_) {
        return;
    }
    shared_audio_session_gate().release(AudioSession::playback);
    session_acquired_ = false;
}

}  // namespace chatesp
