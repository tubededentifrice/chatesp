#pragma once

#include <cstdint>

namespace chatesp {

enum class QuickControlsAction : std::uint8_t {
    none,
    open,
    close,
};

struct QuickControlsConfig {
    std::int16_t top_edge_height_px = 32;
    std::int16_t swipe_distance_px = 48;
    std::int16_t maximum_horizontal_drift_px = 72;
    std::uint32_t automatic_close_ms = 5'000;
};

constexpr bool quick_controls_can_persist(
    bool voice_has_priority,
    bool action_button_is_pressed,
    bool sleep_is_pending) {
    return !voice_has_priority && !action_button_is_pressed &&
        !sleep_is_pending;
}

constexpr QuickControlsAction quick_controls_settle_action(
    std::int32_t panel_y,
    std::int32_t hidden_y,
    std::int32_t shown_y) {
    if (shown_y <= hidden_y) {
        return QuickControlsAction::close;
    }
    const std::int32_t bounded_y = panel_y < hidden_y
        ? hidden_y
        : (panel_y > shown_y ? shown_y : panel_y);
    const std::int64_t deployed =
        static_cast<std::int64_t>(bounded_y) - hidden_y;
    const std::int64_t travel =
        static_cast<std::int64_t>(shown_y) - hidden_y;
    return deployed * 2 >= travel
        ? QuickControlsAction::open
        : QuickControlsAction::close;
}

class QuickControlsGesture {
public:
    explicit QuickControlsGesture(QuickControlsConfig config = {})
        : config_(config) {}

    void set_allowed(bool allowed) {
        allowed_ = allowed;
        pressed_ = false;
        if (!allowed_) {
            open_ = false;
        }
    }

    void set_open(bool open, std::uint32_t now_ms) {
        open_ = allowed_ && open;
        pressed_ = false;
        last_activity_at_ms_ = now_ms;
    }

    void press(
        std::int16_t x, std::int16_t y, std::uint32_t now_ms) {
        pressed_ = allowed_ && (open_ || y <= config_.top_edge_height_px);
        release_was_accepted_ = false;
        start_x_ = x;
        start_y_ = y;
        last_activity_at_ms_ = now_ms;
    }

    QuickControlsAction release(
        std::int16_t x, std::int16_t y, std::uint32_t now_ms) {
        last_activity_at_ms_ = now_ms;
        if (!pressed_) {
            return QuickControlsAction::none;
        }
        pressed_ = false;

        const std::int32_t horizontal =
            static_cast<std::int32_t>(x) - start_x_;
        const std::int32_t vertical =
            static_cast<std::int32_t>(y) - start_y_;
        const std::int32_t horizontal_distance =
            horizontal < 0 ? -horizontal : horizontal;
        if (horizontal_distance > config_.maximum_horizontal_drift_px) {
            return QuickControlsAction::none;
        }
        release_was_accepted_ = true;
        if (!open_ && vertical >= config_.swipe_distance_px) {
            return QuickControlsAction::open;
        }
        if (open_ && vertical <= -config_.swipe_distance_px) {
            return QuickControlsAction::close;
        }
        return QuickControlsAction::none;
    }

    [[nodiscard]] bool automatic_close_due(
        std::uint32_t now_ms) const {
        return open_ && !pressed_ &&
            now_ms - last_activity_at_ms_ >= config_.automatic_close_ms;
    }

    void note_activity(std::uint32_t now_ms) {
        last_activity_at_ms_ = now_ms;
    }

    [[nodiscard]] bool open() const { return open_; }
    [[nodiscard]] bool allowed() const { return allowed_; }
    [[nodiscard]] bool pressed() const { return pressed_; }
    [[nodiscard]] bool release_was_accepted() const {
        return release_was_accepted_;
    }

    [[nodiscard]] std::int32_t drag_distance_y(std::int16_t y) const {
        return pressed_
            ? static_cast<std::int32_t>(y) - start_y_
            : 0;
    }

    [[nodiscard]] static std::uint8_t snap_percent(
        std::int32_t value, std::uint8_t minimum) {
        const std::int32_t bounded = value < minimum
            ? minimum
            : (value > 100 ? 100 : value);
        const std::int32_t snapped = ((bounded + 2) / 5) * 5;
        const std::int32_t result = snapped < minimum
            ? minimum
            : (snapped > 100 ? 100 : snapped);
        return static_cast<std::uint8_t>(result);
    }

    [[nodiscard]] static std::uint8_t percent_for_track_position(
        std::int32_t position_px,
        std::int32_t span_px,
        std::uint8_t minimum) {
        if (span_px <= 0) {
            return minimum;
        }
        const std::int32_t bounded = position_px < 0
            ? 0
            : (position_px > span_px ? span_px : position_px);
        const std::int32_t range = 100 - minimum;
        const std::int32_t percent = minimum +
            (bounded * range + span_px / 2) / span_px;
        return snap_percent(percent, minimum);
    }

private:
    QuickControlsConfig config_;
    std::int16_t start_x_ = 0;
    std::int16_t start_y_ = 0;
    std::uint32_t last_activity_at_ms_ = 0;
    bool allowed_ = false;
    bool open_ = false;
    bool pressed_ = false;
    bool release_was_accepted_ = false;
};

}  // namespace chatesp
