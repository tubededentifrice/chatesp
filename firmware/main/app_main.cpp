#include <cstdint>

#include "bsp/esp-bsp.h"
#include "chatesp/app_mode.hpp"
#include "crash_diagnostics.hpp"
#include "device_write_policy.hpp"
#include "device_memory_store.hpp"
#include "device_preferences_store.hpp"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power_control.hpp"
#include "task_config.hpp"
#include "ui.hpp"
#include "voice_runtime.hpp"

namespace {

constexpr char kTag[] = "chatesp";

#ifndef CHATESP_DEVELOPMENT_MODE
#define CHATESP_DEVELOPMENT_MODE 0
#endif

constexpr bool kDevelopmentMode = CHATESP_DEVELOPMENT_MODE != 0;
constexpr std::uint32_t kSystemOffRetryMs = 1'000;
constexpr std::uint32_t kSystemOffPollMs = 50;

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
    (void)chatesp::ui::sleep(false);
    (void)chatesp::power::power_off();
}

void enforce_startup_battery_limit() {
    const auto status = chatesp::power::battery_status();
    if (!status.has_value() ||
        !chatesp::power::low_battery_requires_shutdown(*status)) {
        return;
    }
    ESP_LOGW(kTag, "Low battery system-off requested at start");
    while (true) {
        const esp_err_t result = chatesp::power::power_off();
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "Low battery system-off request failed");
        }
        vTaskDelay(pdMS_TO_TICKS(kSystemOffRetryMs));
        const auto next_status = chatesp::power::battery_status();
        if (next_status.has_value() &&
            !chatesp::power::low_battery_requires_shutdown(*next_status)) {
            esp_restart();
        }
    }
}

void wait_for_system_off_or_wake(chatesp::VoiceRuntime &runtime) {
    // A connected USB source can keep the ESP32 powered after the AXP2101
    // accepts system-off. Keep the production runtime latched in its cleaned
    // sleep state. A bottom-button press still wakes at once. Otherwise,
    // repeat the request so USB removal cannot leave the device awake.
    std::uint32_t last_request_at_ms = monotonic_ms();
    bool poll_error_reported = false;
    while (runtime.poweroff_ready()) {
        chatesp::ButtonEdges edges;
        const std::uint32_t now_ms = monotonic_ms();
        const esp_err_t poll_result = chatesp::power::poll(now_ms, &edges);
        if (poll_result == ESP_OK) {
            poll_error_reported = false;
            if (edges.pressed) {
                runtime.action_button_edge(true, now_ms);
            }
            if (edges.released) {
                runtime.action_button_edge(
                    false, now_ms, edges.short_press_confirmed);
            }
        } else if (!poll_error_reported) {
            ESP_LOGE(kTag, "Power button read failed during system-off wait");
            poll_error_reported = true;
        }
        if (!runtime.poweroff_ready()) {
            return;
        }
        if (now_ms - last_request_at_ms >= kSystemOffRetryMs) {
            last_request_at_ms = now_ms;
            const esp_err_t result = chatesp::power::power_off();
            if (result != ESP_OK) {
                ESP_LOGE(kTag, "System-off retry failed");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(kSystemOffPollMs));
    }
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
    enforce_startup_battery_limit();
    const bool startup_button_down =
        chatesp::power::action_button_is_pressed();
    const std::uint32_t startup_button_at_ms =
        chatesp::power::startup_button_started_at_ms(monotonic_ms());

    // Complete panel polling transactions before NVS work can let LVGL run.
    // The startup frame stays at zero brightness until the selected value is
    // available.
    if (!chatesp::ui::start_hidden(
            chatesp::task_config::lvgl_port_config())) {
        ESP_LOGE(kTag, "Display hidden start failed");
        return;
    }
    chatesp::crash_diagnostics::mark(
        chatesp::runtime::CrashEvent::startup_panel_ready);

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

    if (!chatesp::ui::publish_startup(
            device_preferences.brightness_percent)) {
        ESP_LOGE(kTag, "Display startup publish failed");
        return;
    }
    chatesp::crash_diagnostics::mark(
        chatesp::runtime::CrashEvent::startup_first_pixel);
    ESP_LOGI(
        kTag,
        "Display ready at %u percent",
        static_cast<unsigned>(device_preferences.brightness_percent));
    chatesp::crash_diagnostics::mark(
        chatesp::runtime::CrashEvent::display_ready);

    if (!chatesp::ui::prepare_capture_views()) {
        ESP_LOGE(kTag, "Capture views start failed");
        return;
    }

    // The voice runtime loads saved memories after capture views are ready.
    // Keep the static store alive for the runtime and its startup worker.
    static chatesp::DeviceMemoryStore device_memory_store;

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
    chatesp::crash_diagnostics::mark(
        chatesp::runtime::CrashEvent::startup_capture_ready);
    ESP_LOGI(kTag, "Voice runtime ready");
    chatesp::crash_diagnostics::mark(
        chatesp::runtime::CrashEvent::runtime_ready);
    ESP_LOGI(
        kTag,
        "Main stack minimum free bytes: %u",
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

    chatesp::ShortPressGesture mode_button;
    chatesp::RestartChordGesture restart_chord;
    bool mode_button_error_reported = false;
    bool power_poll_error_reported = false;
    while (true) {
        chatesp::ButtonEdges edges;
        const std::uint32_t now_ms = monotonic_ms();
        if (chatesp::power::poll(now_ms, &edges) != ESP_OK) {
            if (!power_poll_error_reported) {
                chatesp::crash_diagnostics::mark(
                    chatesp::runtime::CrashEvent::pwr_poll_failed);
                power_poll_error_reported = true;
            }
            ESP_LOGE(kTag, "Power button read failed");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (power_poll_error_reported) {
            chatesp::crash_diagnostics::mark(
                chatesp::runtime::CrashEvent::pwr_poll_recovered);
            power_poll_error_reported = false;
        }
        if (edges.pressed) {
            runtime.action_button_edge(true, now_ms);
        }
        if (edges.released) {
            runtime.action_button_edge(
                false, now_ms, edges.short_press_confirmed);
        }
        chatesp::ButtonEdges mode_edges;
        const esp_err_t mode_result =
            chatesp::power::poll_mode_button(now_ms, &mode_edges);
        if (mode_result == ESP_OK) {
            mode_button_error_reported = false;
            if (mode_edges.pressed) {
                if (runtime.mode_button_available()) {
                    mode_button.press(now_ms);
                    runtime.mode_button_edge(true);
                } else {
                    mode_button.cancel();
                    runtime.mode_button_edge(false);
                }
            }
            if (mode_edges.released) {
                runtime.mode_button_edge(false);
                if (mode_button.release(now_ms)) {
                    ESP_LOGI(kTag, "Top mode button short press accepted");
                    runtime.mode_button_short_press(now_ms);
                }
            }
        } else if (!mode_button_error_reported) {
            ESP_LOGW(kTag, "Top mode button read is not available");
            mode_button_error_reported = true;
            mode_button.cancel();
            runtime.mode_button_edge(false);
        }
        if (restart_chord.update(
                chatesp::power::action_button_is_pressed(),
                chatesp::power::mode_button_is_pressed(), now_ms)) {
            chatesp::crash_diagnostics::mark(
                chatesp::runtime::CrashEvent::restart_button_chord);
            ESP_LOGI(kTag, "PWR and BOOT restart chord accepted");
            esp_restart();
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
                // Battery power normally ends execution in power_off(). USB
                // can keep the MCU alive after the PMIC accepts the request.
                wait_for_system_off_or_wake(runtime);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}
