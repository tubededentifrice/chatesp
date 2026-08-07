#include <cstdint>

#include "bsp/esp-bsp.h"
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

}  // namespace

extern "C" void app_main() {
    ESP_LOGI(
        kTag,
        "Starting application in %s mode",
        kDevelopmentMode ? "development" : "production");

    if (chatesp::power::initialize() != ESP_OK) {
        ESP_LOGE(kTag, "Power button start failed");
        return;
    }
    const bool startup_button_down =
        chatesp::power::action_button_is_pressed();
    const std::uint32_t startup_button_at_ms = monotonic_ms();

    if (!chatesp::ui::start()) {
        ESP_LOGE(kTag, "Display start failed");
        return;
    }

    static chatesp::VoiceRuntime runtime;
    const esp_err_t runtime_result = runtime.start(
        startup_button_down, startup_button_at_ms);
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
                if (failure_press_seen && edges.released) {
                    (void)chatesp::power::power_off();
                }
            }
            if (!automatic_sleep_requested &&
                now_ms - failure_started_ms >= 30'000) {
                automatic_sleep_requested = true;
                (void)chatesp::ui::sleep();
                (void)chatesp::power::power_off();
            }
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }

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
                while (true) {
                    vTaskDelay(pdMS_TO_TICKS(1'000));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}
