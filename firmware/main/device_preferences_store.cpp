#include "device_preferences_store.hpp"

#include <cstring>

#include "nvs.h"
#include "nvs_flash.h"

namespace chatesp {
namespace {

constexpr char kNamespace[] = "chatesp_device";
constexpr char kPreferencesKey[] = "prefs";

}  // namespace

esp_err_t DevicePreferencesStore::initialize() {
    if (initialized_) {
        return ESP_OK;
    }
    const esp_err_t init_result = nvs_flash_init();
    if (init_result != ESP_OK) {
        return init_result;
    }

    runtime::DevicePreferences loaded_preferences;

    nvs_handle_t handle = 0;
    const esp_err_t open_result = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (open_result == ESP_ERR_NVS_NOT_FOUND) {
        preferences_ = loaded_preferences;
        persistent_ = true;
        initialized_ = true;
        return ESP_OK;
    }
    if (open_result != ESP_OK) {
        return open_result;
    }

    std::size_t encoded_size = 0;
    esp_err_t read_result = nvs_get_blob(
        handle, kPreferencesKey, nullptr, &encoded_size);
    if (read_result == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        preferences_ = loaded_preferences;
        persistent_ = true;
        initialized_ = true;
        return ESP_OK;
    }
    if (read_result != ESP_OK) {
        nvs_close(handle);
        return read_result;
    }

    runtime::EncodedDevicePreferences encoded{};
    if (encoded_size != encoded.size()) {
        nvs_close(handle);
        preferences_ = loaded_preferences;
        persistent_ = true;
        initialized_ = true;
        return ESP_OK;
    }
    read_result = nvs_get_blob(
        handle, kPreferencesKey, encoded.data(), &encoded_size);
    nvs_close(handle);
    if (read_result != ESP_OK) {
        return read_result;
    }

    runtime::DevicePreferences decoded;
    if (runtime::decode_device_preferences(
            encoded.data(), encoded_size, decoded)) {
        loaded_preferences = decoded;
    }
    preferences_ = loaded_preferences;
    persistent_ = true;
    initialized_ = true;
    return ESP_OK;
}

bool DevicePreferencesStore::store(
    const runtime::DevicePreferences &preferences) {
    if (!persistent_ || !preferences.valid()) {
        return false;
    }
    if (preferences == preferences_) {
        return true;
    }
    const runtime::EncodedDevicePreferences encoded =
        runtime::encode_device_preferences(preferences);
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t result = nvs_set_blob(
        handle, kPreferencesKey, encoded.data(), encoded.size());
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }

    runtime::EncodedDevicePreferences verification{};
    std::size_t verification_size = verification.size();
    if (result == ESP_OK) {
        result = nvs_get_blob(
            handle, kPreferencesKey, verification.data(),
            &verification_size);
    }
    nvs_close(handle);
    if (result != ESP_OK || verification_size != encoded.size() ||
        std::memcmp(
            encoded.data(), verification.data(), encoded.size()) != 0) {
        return false;
    }
    preferences_ = preferences;
    return true;
}

}  // namespace chatesp
