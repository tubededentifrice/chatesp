#include "chatesp/pcm_start_policy.hpp"

namespace chatesp {
namespace runtime {
namespace {

bool safely_faster_than_playback(
    std::size_t bytes, std::uint32_t elapsed_ms) {
    const std::uint64_t scaled_bytes =
        static_cast<std::uint64_t>(bytes) * 1'000ULL;
    const std::uint64_t required_bytes =
        static_cast<std::uint64_t>(
            AdaptivePcmStartPolicy::kMinimumIngressBytesPerSecond) *
        (elapsed_ms == 0 ? 1ULL : elapsed_ms);
    return scaled_bytes > required_bytes;
}

}  // namespace

void AdaptivePcmStartPolicy::reset() {
    first_byte_at_ms_ = 0;
    total_bytes_ = 0;
    saw_first_byte_ = false;
    decided_ = false;
    streams_early_ = false;
}

void AdaptivePcmStartPolicy::observe(
    std::size_t total_bytes, std::uint32_t now_ms) {
    if (decided_ || total_bytes == 0 || total_bytes < total_bytes_) {
        return;
    }
    total_bytes_ = total_bytes;
    if (!saw_first_byte_) {
        first_byte_at_ms_ = now_ms;
        saw_first_byte_ = true;
    }
    if (total_bytes_ < kPrebufferBytes) {
        return;
    }
    streams_early_ = safely_faster_than_playback(
        total_bytes_, now_ms - first_byte_at_ms_);
    decided_ = true;
}

PcmStartDecision AdaptivePcmStartPolicy::decision(
    bool response_complete) const {
    if (response_complete && total_bytes_ != 0) {
        return PcmStartDecision::start_complete;
    }
    if (decided_ && streams_early_) {
        return PcmStartDecision::stream_now;
    }
    return PcmStartDecision::wait;
}

}  // namespace runtime
}  // namespace chatesp
