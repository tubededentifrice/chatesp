#pragma once

#include <cstddef>
#include <cstdint>

namespace chatesp {
namespace runtime {

enum class PcmStartDecision : std::uint8_t {
    wait,
    stream_now,
    start_complete,
};

// This policy makes one early-start decision after a 200 ms PCM prebuffer.
// A slow decision is final for the response, so later bursts cannot cause an
// underrun after a long initial transfer.
class AdaptivePcmStartPolicy {
public:
    static constexpr std::size_t kPrebufferBytes = 9'600;
    static constexpr std::uint32_t kPlaybackBytesPerSecond = 48'000;
    static constexpr std::uint32_t kMinimumIngressBytesPerSecond =
        kPlaybackBytesPerSecond * 3 / 2;
    static_assert(
        kPrebufferBytes % 2 == 0,
        "The PCM prebuffer must contain complete samples");
    static_assert(
        kMinimumIngressBytesPerSecond > kPlaybackBytesPerSecond,
        "The ingress rate must have playback headroom");

    void reset();
    void observe(std::size_t total_bytes, std::uint32_t now_ms);

    [[nodiscard]] PcmStartDecision decision(
        bool response_complete) const;
    [[nodiscard]] bool decided() const { return decided_; }
    [[nodiscard]] bool streams_early() const {
        return decided_ && streams_early_;
    }

private:
    std::uint32_t first_byte_at_ms_ = 0;
    std::size_t total_bytes_ = 0;
    bool saw_first_byte_ = false;
    bool decided_ = false;
    bool streams_early_ = false;
};

}  // namespace runtime
}  // namespace chatesp
