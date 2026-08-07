#pragma once

#include "chatesp/interaction_state.hpp"
#include "esp_err.h"

namespace chatesp::ui {

bool start();
void show_state(InteractionState state);
esp_err_t sleep();
esp_err_t wake(InteractionState state);

}  // namespace chatesp::ui
