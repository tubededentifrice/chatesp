#pragma once

#include <cstddef>
#include <cstdint>

#include "audio_playback.hpp"
#include "chatesp/agent_interfaces.hpp"
#include "chatesp/pcm16_stream.hpp"

namespace chatesp {

class PcmPlaybackSink final : public agent::PcmSink {
public:
    explicit PcmPlaybackSink(AudioPlayback &playback);

    PcmPlaybackSink(const PcmPlaybackSink &) = delete;
    PcmPlaybackSink &operator=(const PcmPlaybackSink &) = delete;

    agent::Error begin(
        std::uint32_t sample_rate_hz, std::uint8_t channels,
        std::uint8_t bits_per_sample) override;
    agent::Error write(
        const std::uint8_t *data, std::size_t size) override;
    agent::Error finish() override;

    // This operation is safe from the button task.
    void cancel();
    // Call this operation only from the playback owner task.
    agent::Error cancel_and_stop();

private:
    static bool play_samples(
        void *context, const std::int16_t *samples,
        std::size_t sample_count);
    agent::Error stop();

    AudioPlayback &playback_;
    runtime::Pcm16Stream stream_;
    agent::Error output_error_ = agent::Error::none;
    std::size_t byte_count_ = 0;
    std::size_t sample_count_ = 0;
    bool started_ = false;
};

}  // namespace chatesp
