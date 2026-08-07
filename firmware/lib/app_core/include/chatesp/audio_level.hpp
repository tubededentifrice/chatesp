#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace chatesp {

inline std::uint8_t pcm_peak_percent(
    const std::int16_t *samples,
    std::size_t sample_count,
    std::size_t tail_count,
    std::uint32_t full_scale_peak = 12'000) {
    if (samples == nullptr || sample_count == 0 || tail_count == 0 ||
        full_scale_peak == 0) {
        return 0;
    }
    tail_count = std::min(sample_count, tail_count);
    std::uint32_t peak = 0;
    for (std::size_t index = sample_count - tail_count;
         index < sample_count; ++index) {
        const std::int32_t value = samples[index];
        const std::uint32_t magnitude = static_cast<std::uint32_t>(
            value < 0 ? -value : value);
        peak = std::max(peak, magnitude);
    }
    const std::uint64_t scaled =
        static_cast<std::uint64_t>(peak) * 100 / full_scale_peak;
    return static_cast<std::uint8_t>(
        std::min<std::uint64_t>(100, scaled));
}

}  // namespace chatesp
