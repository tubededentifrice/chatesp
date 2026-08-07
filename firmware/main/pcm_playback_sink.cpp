#include "pcm_playback_sink.hpp"

#include "esp_err.h"

namespace chatesp {
namespace {

constexpr int kPlaybackVolumePercent = 70;

}  // namespace

PcmPlaybackSink::PcmPlaybackSink(AudioPlayback &playback)
    : playback_(playback) {}

agent::Error PcmPlaybackSink::begin(
    std::uint32_t sample_rate_hz, std::uint8_t channels,
    std::uint8_t bits_per_sample) {
    if (started_) {
        return agent::Error::invalid_argument;
    }
    if (sample_rate_hz != AudioPlayback::kSampleRateHz ||
        channels != AudioPlayback::kChannelCount ||
        bits_per_sample != AudioPlayback::kBitsPerSample) {
        return agent::Error::unsupported_media;
    }

    stream_.reset();
    output_error_ = agent::Error::none;
    byte_count_ = 0;
    sample_count_ = 0;
    if (playback_.start(kPlaybackVolumePercent) != ESP_OK) {
        return agent::Error::model_failed;
    }
    started_ = true;
    return agent::Error::none;
}

agent::Error PcmPlaybackSink::write(
    const std::uint8_t *data, std::size_t size) {
    if (!started_ || (size != 0 && data == nullptr)) {
        return agent::Error::invalid_argument;
    }
    if (output_error_ != agent::Error::none) {
        return output_error_;
    }
    if (size > agent::Limits::max_tts_pcm_bytes - byte_count_) {
        output_error_ = agent::Error::limit_exceeded;
        return output_error_;
    }
    if (playback_.cancelled()) {
        output_error_ = agent::Error::cancelled;
        return output_error_;
    }

    const runtime::Pcm16Stream::Status status =
        stream_.write(data, size, play_samples, this);
    if (status == runtime::Pcm16Stream::Status::invalid_argument) {
        return agent::Error::invalid_argument;
    }
    if (status == runtime::Pcm16Stream::Status::output_failed) {
        return output_error_ == agent::Error::none
                   ? agent::Error::model_failed
                   : output_error_;
    }
    byte_count_ += size;
    return agent::Error::none;
}

agent::Error PcmPlaybackSink::finish() {
    if (!started_) {
        return agent::Error::invalid_argument;
    }

    agent::Error result = output_error_;
    if (result == agent::Error::none && playback_.cancelled()) {
        result = agent::Error::cancelled;
    }
    if (result == agent::Error::none) {
        const runtime::Pcm16Stream::Status status = stream_.finish();
        if (status == runtime::Pcm16Stream::Status::incomplete_sample ||
            sample_count_ == 0) {
            result = agent::Error::malformed_response;
        } else if (status == runtime::Pcm16Stream::Status::output_failed) {
            result = output_error_ == agent::Error::none
                         ? agent::Error::model_failed
                         : output_error_;
        }
    }

    const agent::Error stop_error = stop();
    return result == agent::Error::none ? stop_error : result;
}

void PcmPlaybackSink::cancel() { playback_.cancel(); }

agent::Error PcmPlaybackSink::cancel_and_stop() {
    playback_.cancel();
    return stop();
}

bool PcmPlaybackSink::play_samples(
    void *context, const std::int16_t *samples,
    std::size_t sample_count) {
    auto *self = static_cast<PcmPlaybackSink *>(context);
    const esp_err_t error = self->playback_.play(samples, sample_count);
    if (error == ESP_OK) {
        self->sample_count_ += sample_count;
        return true;
    }
    self->output_error_ = self->playback_.cancelled()
                              ? agent::Error::cancelled
                              : agent::Error::model_failed;
    return false;
}

agent::Error PcmPlaybackSink::stop() {
    const esp_err_t error = playback_.stop();
    stream_.reset();
    byte_count_ = 0;
    sample_count_ = 0;
    started_ = false;
    return error == ESP_OK ? agent::Error::none
                           : agent::Error::model_failed;
}

}  // namespace chatesp
