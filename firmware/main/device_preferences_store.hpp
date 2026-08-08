#pragma once

#include "chatesp/device_preferences.hpp"
#include "esp_err.h"

namespace chatesp {

class DevicePreferencesStore {
public:
    esp_err_t initialize();

    [[nodiscard]] runtime::DevicePreferences preferences() const {
        return preferences_;
    }
    [[nodiscard]] bool persistent() const { return persistent_; }
    [[nodiscard]] bool store(const runtime::DevicePreferences &preferences);

private:
    runtime::DevicePreferences preferences_{};
    bool initialized_ = false;
    bool persistent_ = false;
};

}  // namespace chatesp
