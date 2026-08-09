#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace chatesp {

enum class AppMode : std::uint8_t {
    chat,
    clock,
};

enum class DisplayOrientation : std::uint8_t {
    chat,
    clock,
};

// A pairing code always uses the normal ChatESP orientation. If pairing starts
// in Clock mode, the display returns to the Clock orientation after pairing.
[[nodiscard]] constexpr DisplayOrientation display_orientation_for(
    AppMode mode, bool pairing_code_visible) {
    return pairing_code_visible || mode == AppMode::chat
        ? DisplayOrientation::chat
        : DisplayOrientation::clock;
}

struct ClockStyle {
    std::uint32_t background_rgb = 0x000000;
    std::uint32_t time_rgb = 0xffffff;
    std::uint32_t seconds_rgb = 0xffffff;
    std::uint16_t corner_radius_px = 48;

    [[nodiscard]] constexpr bool valid() const {
        return background_rgb <= 0xffffff && time_rgb <= 0xffffff &&
            seconds_rgb <= 0xffffff && corner_radius_px >= 16 &&
            corner_radius_px <= 120;
    }
};

struct ClockTime {
    std::uint8_t hour = 0;
    std::uint8_t minute = 0;
    std::uint8_t second = 0;
    std::uint16_t millisecond = 0;

    [[nodiscard]] constexpr bool valid() const {
        return hour <= 23 && minute <= 59 && second <= 59 &&
            millisecond <= 999;
    }
};

struct ClockPathSpan {
    std::uint16_t first = 0;
    std::uint16_t count = 0;
};

// Use one step for each distinct perimeter pixel. Even minutes fill clockwise
// from 12 o'clock. Odd minutes remove pixels in the same direction.
[[nodiscard]] constexpr ClockPathSpan clock_path_span(
    std::uint8_t minute, std::uint32_t millisecond_in_minute,
    std::uint16_t point_count) {
    const std::uint32_t bounded_millisecond =
        millisecond_in_minute > 59'999 ? 59'999 : millisecond_in_minute;
    const std::uint16_t changed = static_cast<std::uint16_t>(
        bounded_millisecond * point_count / 60'000U);
    return (minute & 1U) == 0U
        ? ClockPathSpan{0, changed}
        : ClockPathSpan{
              changed, static_cast<std::uint16_t>(point_count - changed)};
}

[[nodiscard]] constexpr bool clock_path_point_visible(
    std::uint16_t point, ClockPathSpan span) {
    return point >= span.first &&
        point < static_cast<std::uint32_t>(span.first) + span.count;
}

[[nodiscard]] constexpr std::array<char, 6> clock_time_text(
    bool available, ClockTime time) {
    if (!available || !time.valid()) {
        return {'-', '-', ':', '-', '-', '\0'};
    }
    return {
        static_cast<char>('0' + time.hour / 10U),
        static_cast<char>('0' + time.hour % 10U),
        ':',
        static_cast<char>('0' + time.minute / 10U),
        static_cast<char>('0' + time.minute % 10U),
        '\0',
    };
}

class ShortPressGesture {
public:
    explicit constexpr ShortPressGesture(
        std::uint32_t maximum_press_ms = 700,
        std::uint32_t minimum_press_ms = 80)
        : maximum_press_ms_(maximum_press_ms),
          minimum_press_ms_(minimum_press_ms) {}

    void press(std::uint32_t now_ms) {
        pressed_ = true;
        pressed_at_ms_ = now_ms;
    }

    [[nodiscard]] bool release(std::uint32_t now_ms) {
        if (!pressed_) {
            return false;
        }
        pressed_ = false;
        const std::uint32_t duration_ms = now_ms - pressed_at_ms_;
        return duration_ms >= minimum_press_ms_ &&
            duration_ms <= maximum_press_ms_;
    }

    void cancel() { pressed_ = false; }

private:
    std::uint32_t maximum_press_ms_ = 700;
    std::uint32_t minimum_press_ms_ = 80;
    std::uint32_t pressed_at_ms_ = 0;
    bool pressed_ = false;
};

[[nodiscard]] constexpr bool clock_network_shutdown_due(
    bool pending, bool local_time_available,
    std::uint32_t elapsed_ms, std::uint32_t limit_ms) {
    return pending && (local_time_available || elapsed_ms >= limit_ms);
}

}  // namespace chatesp
