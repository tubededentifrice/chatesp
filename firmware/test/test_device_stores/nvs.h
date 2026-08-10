#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

using nvs_handle_t = std::uint32_t;

constexpr int NVS_READONLY = 0;
constexpr int NVS_READWRITE = 1;

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, std::uint8_t *value);
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, std::uint8_t value);
esp_err_t nvs_get_blob(
    nvs_handle_t handle, const char *key, void *value, std::size_t *size);
esp_err_t nvs_set_blob(
    nvs_handle_t handle, const char *key, const void *value, std::size_t size);
esp_err_t nvs_commit(nvs_handle_t handle);
