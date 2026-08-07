#include "audio_capture.hpp"

#include <algorithm>

#include "bsp/esp-bsp.h"
#include "chatesp/audio_session.hpp"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"

namespace chatesp {
namespace {

constexpr float kMicrophoneGainDb = 30.0F;

esp_codec_dev_handle_t codec_handle(void *codec) {
    return static_cast<esp_codec_dev_handle_t>(codec);
}

void clear_samples(std::int16_t *samples, std::size_t sample_count) {
    volatile std::int16_t *cursor = samples;
    while (sample_count > 0) {
        *cursor++ = 0;
        --sample_count;
    }
}

}  // namespace

AudioCapture::AudioCapture() = default;

AudioCapture::~AudioCapture() {
    cancel();
    stop();
    release_buffer();
    if (codec_ != nullptr) {
        esp_codec_dev_delete(codec_handle(codec_));
        codec_ = nullptr;
    }
}

esp_err_t AudioCapture::start() {
    if (active_) {
        return ESP_ERR_INVALID_STATE;
    }

    release_buffer();
    budget_.reset();
    cancelled_.store(false, std::memory_order_release);

    if (!shared_audio_session_gate().try_acquire(AudioSession::capture)) {
        return ESP_ERR_INVALID_STATE;
    }
    session_acquired_ = true;

    samples_ = static_cast<std::int16_t *>(heap_caps_calloc(
        kMaximumSamples, sizeof(std::int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (samples_ == nullptr) {
        release_session();
        return ESP_ERR_NO_MEM;
    }

    if (codec_ == nullptr) {
        codec_ = bsp_audio_codec_microphone_init();
        if (codec_ == nullptr) {
            release_buffer();
            release_session();
            return ESP_FAIL;
        }
    }

    const int gain_result =
        esp_codec_dev_set_in_gain(codec_handle(codec_), kMicrophoneGainDb);
    if (gain_result != ESP_CODEC_DEV_OK) {
        release_buffer();
        release_session();
        return static_cast<esp_err_t>(gain_result);
    }

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
        release_buffer();
        release_session();
        return static_cast<esp_err_t>(open_result);
    }

    active_ = true;
    return ESP_OK;
}

esp_err_t AudioCapture::capture_chunk() {
    if (!active_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cancelled_.load(std::memory_order_acquire)) {
        return ESP_ERR_INVALID_STATE;
    }

    const std::size_t sample_count =
        std::min(kChunkSamples, budget_.remaining());
    if (sample_count == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    const int read_result = esp_codec_dev_read(
        codec_handle(codec_), samples_ + budget_.used(),
        static_cast<int>(sample_count * sizeof(std::int16_t)));
    if (read_result != ESP_CODEC_DEV_OK) {
        return static_cast<esp_err_t>(read_result);
    }
    if (cancelled_.load(std::memory_order_acquire)) {
        clear_samples(samples_ + budget_.used(), sample_count);
        return ESP_ERR_INVALID_STATE;
    }
    if (!budget_.commit(sample_count)) {
        clear_samples(samples_ + budget_.used(), sample_count);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t AudioCapture::stop() {
    esp_err_t result = ESP_OK;
    if (active_) {
        const int close_result = esp_codec_dev_close(codec_handle(codec_));
        if (close_result != ESP_CODEC_DEV_OK) {
            result = static_cast<esp_err_t>(close_result);
        }
        active_ = false;
    }
    release_session();
    if (cancelled_.load(std::memory_order_acquire)) {
        release_buffer();
    }
    return result;
}

void AudioCapture::cancel() {
    cancelled_.store(true, std::memory_order_release);
}

void AudioCapture::discard() {
    cancel();
    stop();
    release_buffer();
    budget_.reset();
}

const std::int16_t *AudioCapture::samples() const { return samples_; }

std::size_t AudioCapture::sample_count() const { return budget_.used(); }

bool AudioCapture::active() const { return active_; }

bool AudioCapture::cancelled() const {
    return cancelled_.load(std::memory_order_acquire);
}

void AudioCapture::release_buffer() {
    if (samples_ == nullptr) {
        return;
    }
    clear_samples(samples_, kMaximumSamples);
    heap_caps_free(samples_);
    samples_ = nullptr;
    budget_.reset();
}

void AudioCapture::release_session() {
    if (!session_acquired_) {
        return;
    }
    shared_audio_session_gate().release(AudioSession::capture);
    session_acquired_ = false;
}

}  // namespace chatesp
