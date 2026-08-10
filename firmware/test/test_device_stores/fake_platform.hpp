#pragma once

#include <cstddef>

#include "esp_err.h"

namespace fake_platform {

void reset();
void fail_mutex_allocation_on_call(std::size_t call);
void set_nvs_init_failure_count(std::size_t count, esp_err_t error);
[[nodiscard]] std::size_t mutex_allocation_count();
[[nodiscard]] std::size_t mutex_delete_count();
[[nodiscard]] std::size_t nvs_init_call_count();

}  // namespace fake_platform
