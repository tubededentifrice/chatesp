#pragma once

#include <cstdint>

#include "chatesp/button_debouncer.hpp"

namespace chatesp {

// The IO-expander PWR level can stay active after a battery-powered release.
// A completed PMU release is authoritative at startup. Keep a held press only
// when the PMU has not reported its release. A USB-removal level needs a new
// PWR press event because both events can occur while the board loses VBUS.
[[nodiscard]] constexpr bool startup_power_button_pressed(
    bool io_level_pressed,
    bool press_event,
    bool release_event,
    bool short_press_event,
    bool vbus_remove_event) {
    if (!io_level_pressed || release_event || short_press_event) {
        return false;
    }
    return !vbus_remove_event || press_event;
}

// The PMU reports PWR-key edges and USB power-source edges in one status byte.
// Reject an ambiguous key edge near a USB event. Require the PMU short-press
// IRQ before a release can request sleep. The IO-expander level is not reliable
// while the board operates from its battery.
class PowerButtonFilter {
public:
    explicit constexpr PowerButtonFilter(
        std::uint32_t debounce_ms = 30,
        std::uint32_t power_source_quarantine_ms = 250)
        : button_(debounce_ms),
          power_source_quarantine_ms_(power_source_quarantine_ms) {}

    void reset(bool pressed, std::uint32_t now_ms) {
        confirmed_pressed_ = pressed;
        short_press_confirmed_ = false;
        power_source_quarantine_active_ = false;
        button_.reset(pressed, now_ms);
    }

    [[nodiscard]] ButtonEdges update(
        bool physical_press_event,
        bool physical_release_event,
        bool power_source_event,
        std::uint32_t now_ms) {
        return update(
            physical_press_event, physical_release_event,
            power_source_event, false, now_ms);
    }

    [[nodiscard]] ButtonEdges update(
        bool physical_press_event,
        bool physical_release_event,
        bool power_source_event,
        bool short_press_event,
        std::uint32_t now_ms) {
        if (power_source_event) {
            power_source_event_at_ms_ = now_ms;
            power_source_quarantine_active_ = true;
        }
        const bool source_quarantined = power_source_quarantine_active_ &&
            now_ms - power_source_event_at_ms_ <
                power_source_quarantine_ms_;
        if (power_source_quarantine_active_ && !source_quarantined) {
            power_source_quarantine_active_ = false;
        }

        // The status byte does not preserve the order of two key edges. Keep
        // the current state if both are present. A normal human press is longer
        // than the 15 ms poll period, so its two edges arrive separately.
        const bool ambiguous_edge = power_source_event ||
            (source_quarantined && physical_press_event &&
             !confirmed_pressed_);
        if (!ambiguous_edge &&
            physical_press_event != physical_release_event) {
            confirmed_pressed_ = physical_press_event;
            if (physical_press_event) {
                short_press_confirmed_ = false;
            } else {
                short_press_confirmed_ = short_press_event;
            }
        } else if (short_press_event && confirmed_pressed_) {
            short_press_confirmed_ = true;
        }

        ButtonEdges edges = button_.update(confirmed_pressed_, now_ms);
        if (edges.released) {
            edges.short_press_confirmed = short_press_confirmed_;
            short_press_confirmed_ = false;
        }
        return edges;
    }

private:
    ButtonDebouncer button_;
    std::uint32_t power_source_quarantine_ms_;
    std::uint32_t power_source_event_at_ms_ = 0;
    bool confirmed_pressed_ = false;
    bool short_press_confirmed_ = false;
    bool power_source_quarantine_active_ = false;
};

}  // namespace chatesp
