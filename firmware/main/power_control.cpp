#include "power_control.hpp"

#include <cstdint>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_io_expander.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace chatesp::power {
namespace {

constexpr char kTag[] = "power";
constexpr std::uint16_t kAxp2101Address = 0x34;
constexpr std::uint8_t kAxp2101CommonConfig = 0x10;
constexpr std::uint8_t kAxp2101SoftwareOff = 1U << 0;
constexpr std::uint8_t kAxp2101PowerKeyShutdown = 1U << 2;
constexpr std::uint32_t kPowerButtonPin = IO_EXPANDER_PIN_NUM_4;
constexpr int kI2cTimeoutMs = 100;

ButtonDebouncer action_button;
esp_io_expander_handle_t io_expander = nullptr;
i2c_master_dev_handle_t axp2101 = nullptr;
bool initialized = false;
bool hardware_hold_shutdown_suppressed = false;

std::uint32_t monotonic_ms() {
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000ULL);
}

esp_err_t read_action_button(bool *pressed) {
    ESP_RETURN_ON_FALSE(
        pressed != nullptr, ESP_ERR_INVALID_ARG, kTag, "No button output");
    std::uint32_t levels = 0;
    ESP_RETURN_ON_ERROR(
        esp_io_expander_get_level(io_expander, kPowerButtonPin, &levels),
        kTag,
        "PWR button read failed");
    *pressed = (levels & kPowerButtonPin) != 0;
    return ESP_OK;
}

esp_err_t read_axp2101(std::uint8_t reg, std::uint8_t *value) {
    return i2c_master_transmit_receive(
        axp2101, &reg, sizeof(reg), value, sizeof(*value), kI2cTimeoutMs);
}

esp_err_t write_axp2101(std::uint8_t reg, std::uint8_t value) {
    const std::uint8_t data[] = {reg, value};
    return i2c_master_transmit(axp2101, data, sizeof(data), kI2cTimeoutMs);
}

esp_err_t set_hardware_hold_shutdown(bool enabled) {
    std::uint8_t common_config = 0;
    ESP_RETURN_ON_ERROR(
        read_axp2101(kAxp2101CommonConfig, &common_config),
        kTag,
        "AXP2101 config read failed");
    const std::uint8_t next = enabled
        ? common_config | kAxp2101PowerKeyShutdown
        : common_config & static_cast<std::uint8_t>(
              ~kAxp2101PowerKeyShutdown);
    ESP_RETURN_ON_ERROR(
        write_axp2101(kAxp2101CommonConfig, next),
        kTag,
        "AXP2101 key setup failed");
    hardware_hold_shutdown_suppressed = !enabled;
    return ESP_OK;
}

}  // namespace

esp_err_t initialize() {
    if (initialized) {
        return ESP_OK;
    }

    io_expander = bsp_io_expander_init();
    ESP_RETURN_ON_FALSE(
        io_expander != nullptr, ESP_FAIL, kTag, "IO expander start failed");
    ESP_RETURN_ON_ERROR(
        esp_io_expander_set_dir(
            io_expander, kPowerButtonPin, IO_EXPANDER_INPUT),
        kTag,
        "PWR button setup failed");

    const i2c_device_config_t axp2101_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = kAxp2101Address,
        .scl_speed_hz = 400'000,
        .scl_wait_us = 0,
        .flags = {},
    };
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(
            bsp_i2c_get_handle(), &axp2101_config, &axp2101),
        kTag,
        "AXP2101 start failed");

    bool pressed = false;
    ESP_RETURN_ON_ERROR(
        read_action_button(&pressed), kTag, "PWR button start read failed");
    if (pressed) {
        ESP_RETURN_ON_ERROR(
            set_hardware_hold_shutdown(false),
            kTag,
            "PWR hold protection failed");
    }
    action_button.reset(pressed, monotonic_ms());
    initialized = true;
    ESP_LOGI(kTag, "Bottom PWR action button ready");
    return ESP_OK;
}

esp_err_t poll(std::uint32_t now_ms, ButtonEdges *edges) {
    ESP_RETURN_ON_FALSE(
        edges != nullptr, ESP_ERR_INVALID_ARG, kTag, "No edge output");
    ESP_RETURN_ON_FALSE(
        initialized, ESP_ERR_INVALID_STATE, kTag, "Power not ready");
    bool pressed = false;
    ESP_RETURN_ON_ERROR(
        read_action_button(&pressed), kTag, "PWR button read failed");
    if (pressed && !hardware_hold_shutdown_suppressed) {
        ESP_RETURN_ON_ERROR(
            set_hardware_hold_shutdown(false),
            kTag,
            "PWR hold protection failed");
    } else if (!pressed && hardware_hold_shutdown_suppressed) {
        ESP_RETURN_ON_ERROR(
            set_hardware_hold_shutdown(true),
            kTag,
            "PWR recovery setup failed");
    }
    *edges = action_button.update(pressed, now_ms);
    return ESP_OK;
}

bool action_button_is_pressed() {
    bool pressed = false;
    return initialized && read_action_button(&pressed) == ESP_OK && pressed;
}

esp_err_t power_off() {
    ESP_RETURN_ON_FALSE(
        initialized, ESP_ERR_INVALID_STATE, kTag, "Power not ready");
    std::uint8_t common_config = 0;
    ESP_RETURN_ON_ERROR(
        read_axp2101(kAxp2101CommonConfig, &common_config),
        kTag,
        "AXP2101 config read failed");
    ESP_LOGI(kTag, "Requesting AXP2101 system off");
    return write_axp2101(
        kAxp2101CommonConfig, common_config | kAxp2101SoftwareOff);
}

}  // namespace chatesp::power
