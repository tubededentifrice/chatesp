#pragma once

#include <cstddef>
#include <cstdint>

#include "chatesp/fixed_text.hpp"

namespace chatesp {
namespace agent {

using UtcMinuteText = FixedText<26>;

// Keeps UTC from the app or a trusted HTTP response and advances it with
// monotonic time. An app-provided offset makes the model-facing value local.
// The model-facing value has minute precision and does not contain seconds.
class UtcClock {
public:
    [[nodiscard]] bool update_from_http_date(
        const char *value, std::size_t size, std::uint32_t observed_at_ms);
    [[nodiscard]] bool update_from_epoch_seconds(
        std::uint64_t epoch_seconds,
        std::int16_t utc_offset_minutes,
        std::uint32_t observed_at_ms);
    [[nodiscard]] bool current_minute(
        std::uint32_t now_ms, UtcMinuteText &output) const;
    [[nodiscard]] bool valid() const { return valid_; }

    [[nodiscard]] static bool valid_minute_text(const char *value);

private:
    std::uint64_t observed_epoch_seconds_ = 0;
    std::uint32_t observed_at_ms_ = 0;
    std::int16_t utc_offset_minutes_ = 0;
    bool has_utc_offset_ = false;
    bool valid_ = false;
};

}  // namespace agent
}  // namespace chatesp
