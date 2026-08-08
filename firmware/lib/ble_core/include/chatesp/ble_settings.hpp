#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "chatesp/provisioning_packet.hpp"

namespace chatesp {
namespace provisioning {

template <std::size_t Capacity>
class BoundedSetting {
public:
    ~BoundedSetting() { clear(); }

    [[nodiscard]] bool assign(std::string_view value) {
        if (value.size() > Capacity) {
            return false;
        }
        clear();
        for (std::size_t index = 0; index < value.size(); ++index) {
            bytes_[index] = value[index];
        }
        size_ = value.size();
        return true;
    }

    [[nodiscard]] std::string_view view() const {
        return {bytes_.data(), size_};
    }

    void clear() {
        volatile char *cursor = bytes_.data();
        for (std::size_t index = 0; index < bytes_.size(); ++index) {
            cursor[index] = 0;
        }
        size_ = 0;
    }

private:
    std::array<char, Capacity + 1> bytes_{};
    std::size_t size_ = 0;
};

struct SettingsRecord {
    ~SettingsRecord() { clear(); }

    std::uint32_t revision = 0;
    std::array<std::uint8_t, kFingerprintSize> fingerprint{};
    BoundedSetting<192> chat_endpoint;
    BoundedSetting<256> openrouter_key;
    BoundedSetting<128> brave_key;
    BoundedSetting<32> wifi_ssid;
    BoundedSetting<63> wifi_password;
    BoundedSetting<96> chat_model;
    BoundedSetting<96> transcription_model;
    BoundedSetting<96> speech_model;
    BoundedSetting<96> approximate_location;
    BoundedSetting<96> english_speech_voice;
    BoundedSetting<96> french_speech_voice;

    [[nodiscard]] bool assign(const ValidationResult &validation);
    void clear();
};

}  // namespace provisioning
}  // namespace chatesp
