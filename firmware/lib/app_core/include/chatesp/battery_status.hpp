#pragma once

#include <cstdint>

namespace chatesp {
namespace power {

struct BatteryStatus {
    std::uint8_t percent = 0;
    bool charging = false;
    bool external_power = false;
    bool available = true;

    constexpr bool operator==(const BatteryStatus &other) const {
        return percent == other.percent && charging == other.charging &&
            external_power == other.external_power &&
            available == other.available;
    }

    constexpr bool operator!=(const BatteryStatus &other) const {
        return !(*this == other);
    }
};

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
