#pragma once

#include <cstdint>

namespace chatesp {
namespace power {

struct BatteryStatus {
    std::uint8_t percent = 0;
    bool charging = false;

    constexpr bool operator==(const BatteryStatus &other) const {
        return percent == other.percent && charging == other.charging;
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

}  // namespace power
}  // namespace chatesp
