#pragma once

#include <cstdint>

namespace chatesp {
namespace power {

// AXP2101 REG 27 bits 1:0 select the PWRON recognition time. The minimum
// reversible value is 128 ms. Keep the IRQ and power-off timing fields.
constexpr std::uint8_t kAxp2101PowerOnLevelMask = 0x03;
constexpr std::uint32_t kAxp2101FastPowerOnMs = 128;

struct StartupPowerWakeEvidence {
    bool power_on_reset = false;
    bool io_level_pressed = false;
    bool press_event = false;
    bool release_event = false;
    bool short_press_event = false;
    bool power_source_event = false;
    bool power_status_valid = false;
    bool external_power_present = false;
};

[[nodiscard]] constexpr std::uint8_t axp2101_fast_power_on_config(
    std::uint8_t current) {
    return current & static_cast<std::uint8_t>(
        ~kAxp2101PowerOnLevelMask);
}

// Give the recognition-time credit only to an unambiguous battery PWR
// power-on. A held level alone can survive an ESP reset and is not proof of a
// cold PWR wake. An unavailable power status is also ambiguous.
[[nodiscard]] constexpr bool confirmed_battery_power_key_wake(
    const StartupPowerWakeEvidence &evidence) {
    return evidence.power_on_reset && evidence.io_level_pressed &&
        evidence.press_event && !evidence.release_event &&
        !evidence.short_press_event && !evidence.power_source_event &&
        evidence.power_status_valid && !evidence.external_power_present;
}

[[nodiscard]] constexpr std::uint32_t credited_startup_button_at_ms(
    std::uint32_t observed_at_ms,
    const StartupPowerWakeEvidence &evidence) {
    return observed_at_ms -
        (confirmed_battery_power_key_wake(evidence)
             ? kAxp2101FastPowerOnMs
             : 0U);
}

}  // namespace power
}  // namespace chatesp
