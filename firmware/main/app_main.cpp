#include <cstdint>

#include "bsp/esp-bsp.h"
#include "chatesp/interaction_state.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power_control.hpp"
#include "ui.hpp"

namespace {

constexpr char kTag[] = "chatesp";
constexpr std::uint32_t kVoicePlaceholderMs = 800;

#ifndef CHATESP_DEVELOPMENT_MODE
#define CHATESP_DEVELOPMENT_MODE 0
#endif

constexpr bool kDevelopmentMode = CHATESP_DEVELOPMENT_MODE != 0;

std::uint32_t monotonic_ms() {
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000ULL);
}

}  // namespace

extern "C" void app_main() {
    ESP_LOGI(
        kTag,
        "Starting application in %s mode",
        kDevelopmentMode ? "development" : "production");

    chatesp::InteractionStateMachine interaction;
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

    const std::uint32_t ready_at_ms = monotonic_ms();
    interaction.ready(ready_at_ms);
    if (startup_button_down) {
        interaction.wake_button_down(startup_button_at_ms);
        interaction.tick(ready_at_ms);
    }
    ESP_LOGI(kTag, "State: %s", chatesp::state_name(interaction.state()));
    if (bsp_display_lock(1000)) {
        chatesp::ui::show_state(interaction.state());
        bsp_display_unlock();
    }

    chatesp::InteractionState previous = interaction.state();
    std::uint32_t transcribing_at_ms = 0;
    while (true) {
        const std::uint32_t now_ms = monotonic_ms();
        chatesp::ButtonEdges edges;
        if (chatesp::power::poll(now_ms, &edges) != ESP_OK) {
            interaction.fail(now_ms);
        } else {
            if (edges.pressed) {
                interaction.button_down(now_ms);
            }
            if (edges.released) {
                interaction.button_up(now_ms);
            }
        }
        interaction.tick(now_ms);
        if (kDevelopmentMode &&
            interaction.state() == chatesp::InteractionState::sleep_pending) {
            interaction.ready(now_ms);
            ESP_LOGI(kTag, "Development mode kept the device awake");
        }
        if (previous == chatesp::InteractionState::transcribing &&
            interaction.state() == chatesp::InteractionState::transcribing &&
            now_ms - transcribing_at_ms >= kVoicePlaceholderMs) {
            interaction.fail(now_ms);
        }
        if (interaction.state() != previous) {
            previous = interaction.state();
            ESP_LOGI(kTag, "State: %s", chatesp::state_name(previous));
            if (bsp_display_lock(100)) {
                chatesp::ui::show_state(previous);
                bsp_display_unlock();
            }
            if (previous == chatesp::InteractionState::transcribing) {
                transcribing_at_ms = now_ms;
            }
            if (previous == chatesp::InteractionState::sleep_pending) {
                vTaskDelay(pdMS_TO_TICKS(250));
                esp_err_t result = chatesp::ui::sleep();
                if (result == ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(40));
                    result = chatesp::power::power_off();
                }

                if (result != ESP_OK) {
                    const std::uint32_t failed_at_ms = monotonic_ms();
                    interaction.fail(failed_at_ms);
                    ESP_LOGE(
                        kTag,
                        "System off failed: %s",
                        esp_err_to_name(result));
                    previous = interaction.state();
                    if (chatesp::ui::wake(previous) != ESP_OK) {
                        ESP_LOGE(kTag, "Display wake failed");
                    }
                } else {
                    while (true) {
                        vTaskDelay(pdMS_TO_TICKS(1000));
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
