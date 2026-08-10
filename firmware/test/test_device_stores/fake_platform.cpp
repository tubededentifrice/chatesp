#include "fake_platform.hpp"

#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "freertos/semphr.h"
#include "nvs.h"

struct FakeSemaphore {
    std::timed_mutex mutex;
};

namespace {

std::mutex state_mutex;
std::size_t failed_mutex_call = 0;
std::size_t mutex_allocations = 0;
std::size_t mutex_deletes = 0;
std::size_t nvs_init_calls = 0;
std::size_t nvs_init_failures = 0;
esp_err_t nvs_init_error = ESP_FAIL;
std::uint32_t next_handle = 1;
std::map<nvs_handle_t, std::string> open_handles;
std::map<std::string, std::map<std::string, std::vector<std::uint8_t>>> nvs_data;

std::vector<std::uint8_t> *find_value(
    nvs_handle_t handle, const char *key) {
    const auto handle_entry = open_handles.find(handle);
    if (handle_entry == open_handles.end() || key == nullptr) {
        return nullptr;
    }
    const auto namespace_entry = nvs_data.find(handle_entry->second);
    if (namespace_entry == nvs_data.end()) {
        return nullptr;
    }
    const auto value_entry = namespace_entry->second.find(key);
    if (value_entry == namespace_entry->second.end()) {
        return nullptr;
    }
    return &value_entry->second;
}

}  // namespace

namespace fake_platform {

void reset() {
    std::lock_guard<std::mutex> lock(state_mutex);
    failed_mutex_call = 0;
    mutex_allocations = 0;
    mutex_deletes = 0;
    nvs_init_calls = 0;
    nvs_init_failures = 0;
    nvs_init_error = ESP_FAIL;
    next_handle = 1;
    open_handles.clear();
    nvs_data.clear();
}

void fail_mutex_allocation_on_call(std::size_t call) {
    std::lock_guard<std::mutex> lock(state_mutex);
    failed_mutex_call = call;
}

void set_nvs_init_failure_count(std::size_t count, esp_err_t error) {
    std::lock_guard<std::mutex> lock(state_mutex);
    nvs_init_failures = count;
    nvs_init_error = error;
}

std::size_t mutex_allocation_count() {
    std::lock_guard<std::mutex> lock(state_mutex);
    return mutex_allocations;
}

std::size_t mutex_delete_count() {
    std::lock_guard<std::mutex> lock(state_mutex);
    return mutex_deletes;
}

std::size_t nvs_init_call_count() {
    std::lock_guard<std::mutex> lock(state_mutex);
    return nvs_init_calls;
}

}  // namespace fake_platform

SemaphoreHandle_t xSemaphoreCreateMutex() {
    std::lock_guard<std::mutex> lock(state_mutex);
    ++mutex_allocations;
    if (mutex_allocations == failed_mutex_call) {
        failed_mutex_call = 0;
        return nullptr;
    }
    return new FakeSemaphore;
}

BaseType_t xSemaphoreTake(
    SemaphoreHandle_t semaphore, TickType_t wait_ticks) {
    if (semaphore == nullptr) {
        return pdFALSE;
    }
    if (wait_ticks == portMAX_DELAY) {
        semaphore->mutex.lock();
        return pdTRUE;
    }
    return semaphore->mutex.try_lock_for(std::chrono::milliseconds(wait_ticks))
        ? pdTRUE
        : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
    if (semaphore == nullptr) {
        return pdFALSE;
    }
    semaphore->mutex.unlock();
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore) {
    if (semaphore == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        ++mutex_deletes;
    }
    delete semaphore;
}

esp_err_t nvs_flash_init() {
    std::lock_guard<std::mutex> lock(state_mutex);
    ++nvs_init_calls;
    if (nvs_init_failures != 0) {
        --nvs_init_failures;
        return nvs_init_error;
    }
    return ESP_OK;
}

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle) {
    if (name == nullptr || handle == nullptr) {
        return ESP_FAIL;
    }
    std::lock_guard<std::mutex> lock(state_mutex);
    const auto namespace_entry = nvs_data.find(name);
    if (mode == NVS_READONLY && namespace_entry == nvs_data.end()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (mode == NVS_READWRITE && namespace_entry == nvs_data.end()) {
        nvs_data[name] = {};
    }
    *handle = next_handle++;
    open_handles[*handle] = name;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) {
    std::lock_guard<std::mutex> lock(state_mutex);
    open_handles.erase(handle);
}

esp_err_t nvs_get_u8(
    nvs_handle_t handle, const char *key, std::uint8_t *value) {
    std::lock_guard<std::mutex> lock(state_mutex);
    const std::vector<std::uint8_t> *stored = find_value(handle, key);
    if (stored == nullptr) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (value == nullptr || stored->size() != 1) {
        return ESP_FAIL;
    }
    *value = (*stored)[0];
    return ESP_OK;
}

esp_err_t nvs_set_u8(
    nvs_handle_t handle, const char *key, std::uint8_t value) {
    std::lock_guard<std::mutex> lock(state_mutex);
    const auto handle_entry = open_handles.find(handle);
    if (handle_entry == open_handles.end() || key == nullptr) {
        return ESP_FAIL;
    }
    nvs_data[handle_entry->second][key] = {value};
    return ESP_OK;
}

esp_err_t nvs_get_blob(
    nvs_handle_t handle, const char *key, void *value, std::size_t *size) {
    if (size == nullptr) {
        return ESP_FAIL;
    }
    std::lock_guard<std::mutex> lock(state_mutex);
    const std::vector<std::uint8_t> *stored = find_value(handle, key);
    if (stored == nullptr) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (value == nullptr) {
        *size = stored->size();
        return ESP_OK;
    }
    if (*size < stored->size()) {
        return ESP_FAIL;
    }
    std::memcpy(value, stored->data(), stored->size());
    *size = stored->size();
    return ESP_OK;
}

esp_err_t nvs_set_blob(
    nvs_handle_t handle, const char *key, const void *value,
    std::size_t size) {
    std::lock_guard<std::mutex> lock(state_mutex);
    const auto handle_entry = open_handles.find(handle);
    if (handle_entry == open_handles.end() || key == nullptr ||
        (value == nullptr && size != 0)) {
        return ESP_FAIL;
    }
    std::vector<std::uint8_t> stored;
    if (size != 0) {
        const auto *bytes = static_cast<const std::uint8_t *>(value);
        stored.assign(bytes, bytes + size);
    }
    nvs_data[handle_entry->second][key] = stored;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t) {
    return ESP_OK;
}
