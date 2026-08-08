#include "chatesp/device_preferences.hpp"

namespace chatesp {
namespace runtime {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'C', 'E', 'D', 'P'};
constexpr std::uint8_t kFormatVersion = 1;

}  // namespace

bool DevicePreferences::valid() const {
    return brightness_percent >= minimum_brightness_percent &&
        brightness_percent <= maximum_percent &&
        volume_percent <= maximum_percent;
}

EncodedDevicePreferences encode_device_preferences(
    const DevicePreferences &preferences) {
    if (!preferences.valid()) {
        return {};
    }
    return {
        kMagic[0], kMagic[1], kMagic[2], kMagic[3], kFormatVersion,
        preferences.brightness_percent, preferences.volume_percent, 0};
}

bool decode_device_preferences(
    const std::uint8_t *data, std::size_t size,
    DevicePreferences &preferences) {
    if (data == nullptr || size != DevicePreferences::encoded_size ||
        data[0] != kMagic[0] || data[1] != kMagic[1] ||
        data[2] != kMagic[2] || data[3] != kMagic[3] ||
        data[4] != kFormatVersion || data[7] != 0) {
        return false;
    }
    const DevicePreferences decoded{data[5], data[6]};
    if (!decoded.valid()) {
        return false;
    }
    preferences = decoded;
    return true;
}

}  // namespace runtime
}  // namespace chatesp
