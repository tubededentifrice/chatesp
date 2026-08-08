#pragma once

#include <cstdint>

#include "chatesp/button_debouncer.hpp"

namespace chatesp {

// The PMU reports PWR-key edges and USB power-source edges in one status byte.
// Reject an ambiguous key edge when the same sample contains a USB event. The
// IO-expander level is not reliable while the board operates from its battery.
class PowerButtonFilter {
public:
    explicit constexpr PowerButtonFilter(std::uint32_t debounce_ms = 30)
        : button_(debounce_ms) {}

    void reset(bool pressed, std::uint32_t now_ms) {
        confirmed_pressed_ = pressed;
        button_.reset(pressed, now_ms);
    }

    [[nodiscard]] ButtonEdges update(
        bool physical_press_event,
        bool physical_release_event,
        bool power_source_event,
        std::uint32_t now_ms) {
        // The status byte does not preserve the order of two key edges. Keep
        // the current state if both are present. A normal human press is longer
        // than the 15 ms poll period, so its two edges arrive separately.
        if (!power_source_event &&
            physical_press_event != physical_release_event) {
            confirmed_pressed_ = physical_press_event;
        }

        return button_.update(confirmed_pressed_, now_ms);
    }

private:
    ButtonDebouncer button_;
    bool confirmed_pressed_ = false;
};

}  // namespace chatesp
