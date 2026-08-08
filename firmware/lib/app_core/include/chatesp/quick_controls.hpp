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

private:
    QuickControlsConfig config_;
    std::int16_t start_x_ = 0;
    std::int16_t start_y_ = 0;
    std::uint32_t last_activity_at_ms_ = 0;
    bool allowed_ = false;
    bool open_ = false;
    bool pressed_ = false;
};

}  // namespace chatesp
