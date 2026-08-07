#include "ui.hpp"

#include "bsp/esp-bsp.h"
#include "lvgl.h"

namespace chatesp::ui {
namespace {

lv_obj_t *status_label = nullptr;
lv_obj_t *hint_label = nullptr;

lv_obj_t *active_screen() {
#if LVGL_VERSION_MAJOR >= 9
    return lv_screen_active();
#else
    return lv_scr_act();
#endif
}

const char *hint(InteractionState state) {
    switch (state) {
        case InteractionState::idle:
            return "HOLD TO TALK";
        case InteractionState::recording:
            return "RELEASE TO SEND";
        case InteractionState::transcribing:
            return "VOICE > TEXT";
        case InteractionState::thinking:
            return "MODEL IS WORKING";
        case InteractionState::tool_work:
            return "GETTING CURRENT DATA";
        case InteractionState::speaking:
            return "PLAYING ANSWER";
        case InteractionState::error:
            return "VOICE SERVICE IS NEXT";
        case InteractionState::sleep_pending:
            return "NEW THREAD ON WAKE";
        case InteractionState::booting:
            return "STARTING";
    }
    return "";
}

void create_screen() {
    lv_obj_t *screen = active_screen();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    status_label = lv_label_create(screen);
    lv_obj_set_width(status_label, 336);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(status_label, 2, LV_PART_MAIN);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_align(status_label, LV_ALIGN_TOP_LEFT, 16, 96);

    hint_label = lv_label_create(screen);
    lv_obj_set_width(hint_label, 336);
    lv_obj_set_style_text_color(hint_label, lv_color_hex(0x777777), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(hint_label, 1, LV_PART_MAIN);
    lv_obj_align(hint_label, LV_ALIGN_TOP_LEFT, 16, 142);
}

}  // namespace

bool start() {
    lv_display_t *display = bsp_display_start();
    if (display == nullptr || bsp_display_backlight_off() != ESP_OK) {
        return false;
    }
    if (!bsp_display_lock(1000)) {
        return false;
    }
    create_screen();
    show_state(InteractionState::booting);
    lv_refr_now(display);
    bsp_display_unlock();
    return bsp_display_brightness_set(65) == ESP_OK;
}

void show_state(InteractionState state) {
    if (status_label == nullptr || hint_label == nullptr) {
        return;
    }
    lv_label_set_text(status_label, state_name(state));
    lv_label_set_text(hint_label, hint(state));
}

esp_err_t sleep() { return bsp_display_backlight_off(); }

esp_err_t wake(InteractionState state) {
    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    show_state(state);
    lv_refr_now(nullptr);
    bsp_display_unlock();
    return bsp_display_brightness_set(65);
}

}  // namespace chatesp::ui
