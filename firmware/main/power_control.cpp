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
constexpr std::uint8_t kAxp2101Status1 = 0x00;
constexpr std::uint8_t kAxp2101IrqEnable2 = 0x41;
constexpr std::uint8_t kAxp2101IrqStatus2 = 0x49;
constexpr std::uint8_t kAxp2101BatteryPercent = 0xA4;
constexpr std::uint8_t kAxp2101BatteryPresent = 1U << 3;
constexpr std::uint8_t kAxp2101SoftwareOff = 1U << 0;
constexpr std::uint8_t kAxp2101PowerKeyShutdown = 1U << 2;
constexpr std::uint8_t kAxp2101PowerKeyPositiveEdge = 1U << 0;
constexpr std::uint8_t kAxp2101PowerKeyNegativeEdge = 1U << 1;
constexpr std::uint8_t kAxp2101PowerKeyEdgeMask =
    kAxp2101PowerKeyPositiveEdge | kAxp2101PowerKeyNegativeEdge;
constexpr std::uint8_t kAxp2101VbusRemoveEvent = 1U << 6;
constexpr std::uint8_t kAxp2101VbusInsertEvent = 1U << 7;
constexpr std::uint8_t kAxp2101PowerSourceEventMask =
    kAxp2101VbusRemoveEvent | kAxp2101VbusInsertEvent;
constexpr std::uint32_t kPowerButtonPin = IO_EXPANDER_PIN_NUM_4;
constexpr int kI2cTimeoutMs = 100;
constexpr std::uint32_t kPolicyErrorLogIntervalMs = 1'000;

PowerButtonFilter action_button;
esp_io_expander_handle_t io_expander = nullptr;
i2c_master_dev_handle_t axp2101 = nullptr;
bool initialized = false;
bool hardware_hold_shutdown_suppressed = false;
bool action_button_stably_pressed = false;
bool hold_policy_error_reported = false;
std::uint32_t hold_policy_error_reported_at_ms = 0;

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
    esp_err_t error = read_axp2101(kAxp2101CommonConfig, &common_config);
    if (error != ESP_OK) {
        return error;
    }
    const std::uint8_t next = enabled
        ? common_config | kAxp2101PowerKeyShutdown
        : common_config & static_cast<std::uint8_t>(
              ~kAxp2101PowerKeyShutdown);
    error = write_axp2101(kAxp2101CommonConfig, next);
    if (error != ESP_OK) {
        return error;
    }
    hardware_hold_shutdown_suppressed = !enabled;
    return ESP_OK;
}

esp_err_t prepare_power_key_events(
    bool *startup_press_event, bool *startup_vbus_remove_event) {
    ESP_RETURN_ON_FALSE(
        startup_press_event != nullptr &&
            startup_vbus_remove_event != nullptr,
        ESP_ERR_INVALID_ARG,
        kTag,
        "No startup PWR event output");
    std::uint8_t enabled = 0;
    esp_err_t error = read_axp2101(kAxp2101IrqEnable2, &enabled);
    if (error != ESP_OK) {
        return error;
    }
    enabled |= kAxp2101PowerKeyEdgeMask | kAxp2101PowerSourceEventMask;
    error = write_axp2101(kAxp2101IrqEnable2, enabled);
    if (error != ESP_OK) {
        return error;
    }

    std::uint8_t status = 0;
    error = read_axp2101(kAxp2101IrqStatus2, &status);
    if (error != ESP_OK) {
        return error;
    }
    *startup_press_event =
        (status & kAxp2101PowerKeyNegativeEdge) != 0;
    *startup_vbus_remove_event =
        (status & kAxp2101VbusRemoveEvent) != 0;
    // IRQ status bits are write-one-to-clear. Clear only observed events so an
    // event that arrives after the read stays available to the first poll.
    const std::uint8_t observed = status &
        (kAxp2101PowerKeyEdgeMask | kAxp2101PowerSourceEventMask);
    return observed == 0
        ? ESP_OK
        : write_axp2101(kAxp2101IrqStatus2, observed);
}

esp_err_t read_power_key_events(
    bool *pressed_event,
    bool *released_event,
    bool *power_source_event) {
    ESP_RETURN_ON_FALSE(
        pressed_event != nullptr && released_event != nullptr &&
            power_source_event != nullptr,
        ESP_ERR_INVALID_ARG,
        kTag,
        "No PWR event output");
    std::uint8_t status = 0;
    esp_err_t error = read_axp2101(kAxp2101IrqStatus2, &status);
    if (error != ESP_OK) {
        return error;
    }
    const std::uint8_t observed = status &
        (kAxp2101PowerKeyEdgeMask | kAxp2101PowerSourceEventMask);
    if (observed != 0) {
        error = write_axp2101(kAxp2101IrqStatus2, observed);
        if (error != ESP_OK) {
            return error;
        }
    }
    // PWRON is active low: its negative electrical edge is a press and its
    // positive electrical edge is a release.
    *pressed_event =
        (status & kAxp2101PowerKeyNegativeEdge) != 0;
    *released_event =
        (status & kAxp2101PowerKeyPositiveEdge) != 0;
    *power_source_event =
        (status & kAxp2101PowerSourceEventMask) != 0;
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
    bool startup_press_event = false;
    bool startup_vbus_remove_event = false;
    ESP_RETURN_ON_ERROR(
        prepare_power_key_events(
            &startup_press_event, &startup_vbus_remove_event),
        kTag,
        "PWR event setup failed");

    bool level_pressed = false;
    ESP_RETURN_ON_ERROR(
        read_action_button(&level_pressed),
        kTag,
        "PWR button start read failed");
    const bool pressed = level_pressed &&
        (!startup_vbus_remove_event || startup_press_event);
    if (level_pressed && !pressed) {
        ESP_LOGW(
            kTag,
            "Ignored PWR level after USB removal without a PWR edge");
    }
    if (pressed) {
        const esp_err_t policy_error = set_hardware_hold_shutdown(false);
        if (policy_error != ESP_OK) {
            ESP_LOGE(
                kTag,
                "PWR hold policy update failed (category %s)",
                esp_err_to_name(policy_error));
            hold_policy_error_reported = true;
            hold_policy_error_reported_at_ms = monotonic_ms();
        }
    }
    action_button.reset(pressed, monotonic_ms());
    action_button_stably_pressed = pressed;
    initialized = true;
    ESP_LOGI(kTag, "Bottom PWR action button ready");
    return ESP_OK;
}

esp_err_t poll(std::uint32_t now_ms, ButtonEdges *edges) {
    ESP_RETURN_ON_FALSE(
        edges != nullptr, ESP_ERR_INVALID_ARG, kTag, "No edge output");
    ESP_RETURN_ON_FALSE(
        initialized, ESP_ERR_INVALID_STATE, kTag, "Power not ready");
    bool pressed_event = false;
    bool released_event = false;
    bool power_source_event = false;
    ESP_RETURN_ON_ERROR(
        read_power_key_events(
            &pressed_event, &released_event, &power_source_event),
        kTag,
        "PWR event read failed");
    *edges = action_button.update(
        pressed_event, released_event, power_source_event, now_ms);
    if (edges->pressed) {
        action_button_stably_pressed = true;
    } else if (edges->released) {
        action_button_stably_pressed = false;
    }

    // Return a new edge without doing PMU I2C work. The next poll applies the
    // long-hold policy. This keeps the user action ahead of PMU maintenance.
    if (edges->pressed || edges->released) {
        return ESP_OK;
    }

    // Hold protection is a safety policy around the user input. A temporary
    // PMU I2C error must not remove an edge that was already sampled from the
    // action button. Keep the policy state unchanged so the next poll retries.
    esp_err_t policy_error = ESP_OK;
    if (action_button_stably_pressed &&
        !hardware_hold_shutdown_suppressed) {
        policy_error = set_hardware_hold_shutdown(false);
    } else if (!action_button_stably_pressed &&
               hardware_hold_shutdown_suppressed) {
        policy_error = set_hardware_hold_shutdown(true);
    }
    if (policy_error != ESP_OK) {
        if (!hold_policy_error_reported ||
            now_ms - hold_policy_error_reported_at_ms >=
                kPolicyErrorLogIntervalMs) {
            ESP_LOGE(
                kTag,
                "PWR hold policy update failed (category %s)",
                esp_err_to_name(policy_error));
            hold_policy_error_reported = true;
            hold_policy_error_reported_at_ms = now_ms;
        }
    } else {
        hold_policy_error_reported = false;
    }
    return ESP_OK;
}

bool action_button_is_pressed() {
    return initialized && action_button_stably_pressed;
}

std::optional<std::uint8_t> battery_percent() {
    if (!initialized) {
        return std::nullopt;
    }
    std::uint8_t status = 0;
    if (read_axp2101(kAxp2101Status1, &status) != ESP_OK ||
        (status & kAxp2101BatteryPresent) == 0) {
        return std::nullopt;
    }
    std::uint8_t percent = 0;
    if (read_axp2101(kAxp2101BatteryPercent, &percent) != ESP_OK ||
        percent > 100) {
        return std::nullopt;
    }
    return percent;
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
