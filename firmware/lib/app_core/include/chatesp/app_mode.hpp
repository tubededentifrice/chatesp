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
    std::uint8_t edge_inset_px = 10;
    std::uint8_t seconds_width_px = 12;

    [[nodiscard]] constexpr bool valid() const {
        return background_rgb <= 0xffffff && time_rgb <= 0xffffff &&
            seconds_rgb <= 0xffffff && corner_radius_px >= 16 &&
            corner_radius_px <= 120 && edge_inset_px >= 4 &&
            edge_inset_px <= 24 && seconds_width_px >= 4 &&
            seconds_width_px <= 20;
    }
};

struct ClockTime {
    std::uint8_t hour = 0;
    std::uint8_t minute = 0;
    std::uint8_t second = 0;

    [[nodiscard]] constexpr bool valid() const {
        return hour <= 23 && minute <= 59 && second <= 59;
    }
};

struct ClockSnakeSpan {
    std::uint8_t first = 0;
    std::uint8_t count = 0;
};

// Use one perimeter section for each second. Even minutes fill clockwise from
// 12 o'clock. Odd minutes drain in the same direction.
[[nodiscard]] constexpr ClockSnakeSpan clock_snake_span(
    std::uint8_t minute, std::uint8_t second) {
    const std::uint8_t bounded_second = second > 59 ? 59 : second;
    const std::uint8_t changed = static_cast<std::uint8_t>(bounded_second + 1);
    return (minute & 1U) == 0U
        ? ClockSnakeSpan{0, changed}
        : ClockSnakeSpan{
              changed, static_cast<std::uint8_t>(60U - changed)};
}

[[nodiscard]] constexpr bool clock_snake_section_visible(
    std::uint8_t section, ClockSnakeSpan span) {
    return section >= span.first &&
        section < static_cast<std::uint8_t>(span.first + span.count);
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

[[nodiscard]] constexpr bool clock_return_due(
    AppMode mode, bool return_pending, bool interaction_idle,
    std::uint32_t inactivity_ms, std::uint32_t delay_ms = 30'000) {
    return mode == AppMode::chat && return_pending && interaction_idle &&
        inactivity_ms >= delay_ms;
}

[[nodiscard]] constexpr bool clock_network_shutdown_due(
    bool pending, bool local_time_available,
    std::uint32_t elapsed_ms, std::uint32_t limit_ms) {
    return pending && (local_time_available || elapsed_ms >= limit_ms);
}

}  // namespace chatesp
