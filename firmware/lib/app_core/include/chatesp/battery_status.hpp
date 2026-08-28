#pragma once

#include <cstdint>

namespace chatesp {
namespace power {

struct BatteryStatus {
    std::uint8_t percent = 0;
    bool charging = false;
    bool external_power = false;
    bool available = true;
    std::uint16_t millivolts = 0;
    bool millivolts_available = false;

    constexpr bool operator==(const BatteryStatus &other) const {
        return percent == other.percent && charging == other.charging &&
            external_power == other.external_power &&
            available == other.available && millivolts == other.millivolts &&
            millivolts_available == other.millivolts_available;
    }

    constexpr bool operator!=(const BatteryStatus &other) const {
        return !(*this == other);
    }
};

// AXP2101 VBAT registers 0x34 and 0x35 contain one 13-bit millivolt value.
// The upper three bits in the high register are not part of the value.
constexpr std::uint16_t axp2101_battery_millivolts(
    std::uint8_t high, std::uint8_t low) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(high & 0x1fU) << 8U) | low);
}

// Raw voltage is diagnostic data. It must not cause a footer redraw when the
// user-visible battery fields did not change.
constexpr bool same_battery_presentation(
    const BatteryStatus &left, const BatteryStatus &right) {
    return left.percent == right.percent &&
        left.charging == right.charging &&
        left.external_power == right.external_power &&
        left.available == right.available;
}

constexpr bool development_sleep_battery_sample_due(
    bool soft_sleep_active,
    bool input_has_priority,
    std::uint32_t last_sample_at_ms,
    std::uint32_t now_ms,
    std::uint32_t interval_ms = 30'000) {
    return soft_sleep_active && !input_has_priority && interval_ms > 0 &&
        now_ms - last_sample_at_ms >= interval_ms;
}

constexpr std::uint16_t saturating_battery_sample_failure_count(
    std::uint16_t current) {
    return current == 0xffffU
        ? current
        : static_cast<std::uint16_t>(current + 1U);
}

// AXP2101 PMU status2 bits 6:5 report the battery current direction.
// Only 01 means that current is charging the battery.
constexpr bool axp2101_status_is_charging(std::uint8_t status2) {
    constexpr std::uint8_t direction_mask = 0x60;
    constexpr std::uint8_t charging_direction = 0x20;
    return (status2 & direction_mask) == charging_direction;
}

constexpr bool axp2101_status_has_external_power(std::uint8_t status1) {
    constexpr std::uint8_t vbus_good = 1U << 5;
    return (status1 & vbus_good) != 0;
}

// VBUS event bits are write-one-to-clear. Keep the prior state when one poll
// contains both events because their order is not available.
constexpr bool external_power_after_events(
    bool prior_connected, bool inserted, bool removed) {
    if (inserted == removed) {
        return prior_connected;
    }
    return inserted;
}

// VBUS good is the primary cable indication. Active battery charge current is
// also sufficient evidence when the PMU VBUS indication is late or transient.
constexpr bool connected_to_external_power(const BatteryStatus &status) {
    return status.external_power || status.charging;
}

constexpr std::uint8_t kLowBatteryShutdownPercent = 5;

constexpr bool low_battery_requires_shutdown(
    const BatteryStatus &status,
    std::uint8_t limit_percent = kLowBatteryShutdownPercent) {
    return status.available && limit_percent <= 100 &&
        status.percent <= limit_percent &&
        !status.external_power && !status.charging;
}

}  // namespace power
}  // namespace chatesp
