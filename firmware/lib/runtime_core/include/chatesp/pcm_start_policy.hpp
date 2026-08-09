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

// This policy starts a fast response after a 200 ms PCM prebuffer. A response
// with a slow first burst gets one later decision with a 500 ms safety buffer.
// A response that is still slower than playback stays buffered to avoid an
// audible underrun.
class AdaptivePcmStartPolicy {
public:
    static constexpr std::size_t kFastPrebufferBytes = 9'600;
    static constexpr std::size_t kSteadyPrebufferBytes = 24'000;
    static constexpr std::size_t kPrebufferBytes = kSteadyPrebufferBytes;
    static constexpr std::uint32_t kPlaybackBytesPerSecond = 48'000;
    static constexpr std::uint32_t kFastIngressBytesPerSecond =
        kPlaybackBytesPerSecond * 3 / 2;
    static constexpr std::uint32_t kSteadyIngressBytesPerSecond =
        kPlaybackBytesPerSecond * 6 / 5;
    static_assert(
        kFastPrebufferBytes % 2 == 0 && kSteadyPrebufferBytes % 2 == 0,
        "PCM prebuffers must contain complete samples");
    static_assert(
        kFastIngressBytesPerSecond > kSteadyIngressBytesPerSecond &&
            kSteadyIngressBytesPerSecond > kPlaybackBytesPerSecond,
        "PCM ingress rates must keep playback headroom");

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
