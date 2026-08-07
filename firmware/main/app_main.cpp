#include <cstdint>

#include "bsp/esp-bsp.h"
#include "chatesp/interaction_state.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui.hpp"

namespace {

constexpr char kTag[] = "chatesp";

std::uint32_t monotonic_ms() {
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000ULL);
}

}  // namespace

extern "C" void app_main() {
    ESP_LOGI(kTag, "Starting application");

    chatesp::InteractionStateMachine interaction;
    if (!chatesp::ui::start()) {
        ESP_LOGE(kTag, "Display start failed");
        return;
    }

    interaction.ready(monotonic_ms());
    ESP_LOGI(kTag, "State: %s", chatesp::state_name(interaction.state()));
    if (bsp_display_lock(1000)) {
        chatesp::ui::show_state(interaction.state());
        bsp_display_unlock();
    }

    chatesp::InteractionState previous = interaction.state();
    while (true) {
        interaction.tick(monotonic_ms());
        if (interaction.state() != previous) {
            previous = interaction.state();
            ESP_LOGI(kTag, "State: %s", chatesp::state_name(previous));
            if (bsp_display_lock(100)) {
                chatesp::ui::show_state(previous);
                bsp_display_unlock();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
