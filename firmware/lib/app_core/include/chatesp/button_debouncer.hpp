#pragma once

#include <cstdint>

namespace chatesp {

struct ButtonEdges {
    bool pressed = false;
    bool released = false;
    bool short_press_confirmed = false;
};

class ButtonDebouncer {
public:
    explicit constexpr ButtonDebouncer(std::uint32_t debounce_ms = 30)
        : debounce_ms_(debounce_ms) {}

    void reset(bool pressed, std::uint32_t now_ms) {
        stable_pressed_ = pressed;
        candidate_pressed_ = pressed;
        candidate_since_ms_ = now_ms;
    }

    [[nodiscard]] ButtonEdges update(
        bool pressed, std::uint32_t now_ms) {
        if (pressed != candidate_pressed_) {
            candidate_pressed_ = pressed;
            candidate_since_ms_ = now_ms;
        }
        if (candidate_pressed_ == stable_pressed_ ||
            now_ms - candidate_since_ms_ < debounce_ms_) {
            return {};
        }

        stable_pressed_ = candidate_pressed_;
        return stable_pressed_ ? ButtonEdges{true, false, false}
                               : ButtonEdges{false, true, false};
    }

private:
    std::uint32_t debounce_ms_;
    std::uint32_t candidate_since_ms_ = 0;
    bool stable_pressed_ = false;
    bool candidate_pressed_ = false;
};

}  // namespace chatesp
