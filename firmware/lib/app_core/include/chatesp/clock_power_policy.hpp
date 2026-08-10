#pragma once

#include <cstdint>

namespace chatesp {
namespace power {

constexpr std::uint32_t kClockUnpoweredSleepMs = 5 * 60'000;

constexpr bool clock_unpowered_sleep_due(
    bool external_power_connected,
    std::uint32_t unpowered_elapsed_ms,
    std::uint32_t timeout_ms = kClockUnpoweredSleepMs) {
    return !external_power_connected && timeout_ms != 0 &&
        unpowered_elapsed_ms >= timeout_ms;
}

}  // namespace power
}  // namespace chatesp
