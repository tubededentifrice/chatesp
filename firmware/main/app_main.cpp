#include <cstdint>

#include "bsp/esp-bsp.h"
#include "chatesp/app_mode.hpp"
#include "crash_diagnostics.hpp"
#include "device_write_policy.hpp"
#include "device_memory_store.hpp"
#include "device_preferences_store.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power_control.hpp"
#include "ui.hpp"
#include "voice_runtime.hpp"

namespace {

constexpr char kTag[] = "chatesp";

#ifndef CHATESP_DEVELOPMENT_MODE
#define CHATESP_DEVELOPMENT_MODE 0
#endif

constexpr bool kDevelopmentMode = CHATESP_DEVELOPMENT_MODE != 0;

std::uint32_t monotonic_ms() {
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1'000ULL);
}

void show_start_error(const char *message) {
    if (bsp_display_lock(100)) {
        chatesp::ui::show_error(message);
        bsp_display_unlock();
    }
}

void request_failure_sleep() {
    (void)chatesp::ui::sleep();
    (void)chatesp::power::power_off();
}

}  // namespace

extern "C" void app_main() {
    chatesp::crash_diagnostics::initialize();
    ESP_LOGI(
        kTag,
        "Starting application in %s mode",
        kDevelopmentMode ? "development" : "production");

    if (chatesp::power::initialize() != ESP_OK) {
        ESP_LOGE(kTag, "Power button start failed");
        return;
    }
    chatesp::crash_diagnostics::mark(
        chatesp::runtime::CrashEvent::power_ready);
    const bool startup_button_down =
        chatesp::power::action_button_is_pressed();
    const std::uint32_t startup_button_at_ms = monotonic_ms();

    static chatesp::DevicePreferencesStore device_preferences_store;
    const esp_err_t preferences_result =
        device_preferences_store.initialize();
    if (preferences_result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "Device preferences are temporary (category %s)",
            esp_err_to_name(preferences_result));
    }
    const chatesp::runtime::DevicePreferences device_preferences =
        device_preferences_store.preferences();

    static chatesp::DeviceMemoryStore device_memory_store;
    const esp_err_t memory_result = device_memory_store.initialize();
    if (memory_result != ESP_OK) {
        ESP_LOGW(
            kTag,
            "Saved memories are unavailable (category %s)",
            esp_err_to_name(memory_result));
    }

    if (!chatesp::ui::start(device_preferences.brightness_percent)) {
        ESP_LOGE(kTag, "Display start failed");
        return;
    }
    ESP_LOGI(
        kTag,
        "Display ready at %u percent",
        static_cast<unsigned>(device_preferences.brightness_percent));
    chatesp::crash_diagnostics::mark(
        chatesp::runtime::CrashEvent::display_ready);

    static chatesp::VoiceRuntime runtime;
    const esp_err_t runtime_result = runtime.start(
        startup_button_down, startup_button_at_ms,
        device_preferences_store, device_memory_store);
    if (runtime_result != ESP_OK) {
        ESP_LOGE(kTag, "Voice runtime start failed");
        show_start_error("VOICE RUNTIME COULD NOT START");
        bool failure_press_seen = startup_button_down;
        bool automatic_sleep_requested = false;
        const std::uint32_t failure_started_ms = monotonic_ms();
        while (true) {
            chatesp::ButtonEdges edges;
            const std::uint32_t now_ms = monotonic_ms();
            if (chatesp::power::poll(now_ms, &edges) == ESP_OK) {
                failure_press_seen = failure_press_seen || edges.pressed;
                if (!kDevelopmentMode && failure_press_seen &&
                    edges.released) {
                    request_failure_sleep();
                }
            }
            if (!kDevelopmentMode && !automatic_sleep_requested &&
                now_ms - failure_started_ms >= 30'000) {
                automatic_sleep_requested = true;
                request_failure_sleep();
            }
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
    ESP_LOGI(kTag, "Voice runtime ready");
    chatesp::crash_diagnostics::mark(
        chatesp::runtime::CrashEvent::runtime_ready);
    ESP_LOGI(
        kTag,
        "Main stack minimum free bytes: %u",
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

    chatesp::ShortPressGesture mode_button;
    bool mode_button_error_reported = false;
    while (true) {
        chatesp::ButtonEdges edges;
        const std::uint32_t now_ms = monotonic_ms();
        if (chatesp::power::poll(now_ms, &edges) != ESP_OK) {
            ESP_LOGE(kTag, "Power button read failed");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (edges.pressed) {
            runtime.action_button_edge(true, now_ms);
        }
        if (edges.released) {
            runtime.action_button_edge(false, now_ms);
        }
        chatesp::ButtonEdges mode_edges;
        const esp_err_t mode_result =
            chatesp::power::poll_mode_button(now_ms, &mode_edges);
        if (mode_result == ESP_OK) {
            mode_button_error_reported = false;
            if (mode_edges.pressed) {
                if (runtime.mode_button_available()) {
                    mode_button.press(now_ms);
                } else {
                    mode_button.cancel();
                }
            }
            if (mode_edges.released && mode_button.release(now_ms)) {
                ESP_LOGI(kTag, "Top mode button short press accepted");
                runtime.mode_button_short_press(now_ms);
            }
        } else if (!mode_button_error_reported) {
            ESP_LOGW(kTag, "Top mode button read is not available");
            mode_button_error_reported = true;
            mode_button.cancel();
        }
        if (runtime.poweroff_ready()) {
            if (chatesp::power::action_button_is_pressed()) {
                runtime.action_button_edge(true, now_ms);
                vTaskDelay(pdMS_TO_TICKS(15));
                continue;
            }
            const esp_err_t result = chatesp::power::power_off();
            if (result != ESP_OK) {
                ESP_LOGE(kTag, "System off failed");
                runtime.poweroff_failed();
            } else {
                // If AXP2101 system-off succeeds, execution stops. USB power
                // can keep the MCU alive, so recover instead of becoming inert.
                const std::uint32_t off_requested_at_ms = monotonic_ms();
                while (monotonic_ms() - off_requested_at_ms < 1'000) {
                    chatesp::ButtonEdges recovery_edges;
                    const std::uint32_t recovery_now_ms = monotonic_ms();
                    if (chatesp::power::poll(
                            recovery_now_ms, &recovery_edges) == ESP_OK) {
                        if (recovery_edges.pressed) {
                            runtime.action_button_edge(
                                true, recovery_now_ms);
                        }
                        if (recovery_edges.released) {
                            runtime.action_button_edge(
                                false, recovery_now_ms);
                        }
                    }
                    if (!runtime.poweroff_ready()) {
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(15));
                }
                if (runtime.poweroff_ready()) {
                    ESP_LOGE(kTag, "System off did not remove power");
                    runtime.poweroff_failed();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}
