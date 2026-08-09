#pragma once

#include <cstdint>

namespace chatesp {
namespace power {

// AXP2101 REG 27 bits 1:0 select the PWRON recognition time. The minimum
// reversible value is 128 ms. Keep the IRQ and power-off timing fields.
constexpr std::uint8_t kAxp2101PowerOnLevelMask = 0x03;
constexpr std::uint32_t kAxp2101FastPowerOnMs = 128;

[[nodiscard]] constexpr std::uint8_t axp2101_fast_power_on_config(
    std::uint8_t current) {
    return current & static_cast<std::uint8_t>(
        ~kAxp2101PowerOnLevelMask);
}

}  // namespace power
}  // namespace chatesp
