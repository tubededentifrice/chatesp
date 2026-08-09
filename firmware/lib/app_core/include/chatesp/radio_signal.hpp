#pragma once

#include <cstdint>

namespace chatesp {
namespace radio {

// Band 1 is strong, band 2 is usable, band 3 is weak, and zero is unknown.
constexpr std::uint8_t signal_band_from_rssi(std::int16_t rssi_dbm) {
    return rssi_dbm >= -65 ? 1 : (rssi_dbm >= -75 ? 2 : 3);
}

}  // namespace radio
}  // namespace chatesp
