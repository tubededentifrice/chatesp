#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace chatesp {
namespace runtime {

struct DevicePreferences {
    static constexpr std::uint8_t default_brightness_percent = 65;
    static constexpr std::uint8_t default_volume_percent = 70;
    static constexpr std::uint8_t minimum_brightness_percent = 5;
    static constexpr std::uint8_t maximum_percent = 100;
    static constexpr std::size_t encoded_size = 8;

    std::uint8_t brightness_percent = default_brightness_percent;
    std::uint8_t volume_percent = default_volume_percent;

    [[nodiscard]] bool valid() const;
    [[nodiscard]] bool operator==(const DevicePreferences &other) const {
        return brightness_percent == other.brightness_percent &&
            volume_percent == other.volume_percent;
    }
    [[nodiscard]] bool operator!=(const DevicePreferences &other) const {
        return !(*this == other);
    }
};

using EncodedDevicePreferences =
    std::array<std::uint8_t, DevicePreferences::encoded_size>;

[[nodiscard]] EncodedDevicePreferences encode_device_preferences(
    const DevicePreferences &preferences);
[[nodiscard]] bool decode_device_preferences(
    const std::uint8_t *data, std::size_t size,
    DevicePreferences &preferences);

}  // namespace runtime
}  // namespace chatesp
