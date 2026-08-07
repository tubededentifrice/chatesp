#include "ui.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <string_view>
#include <utility>

#include "bsp/esp-bsp.h"
#include "lvgl.h"

LV_FONT_DECLARE(chatesp_font_18);

namespace chatesp::ui {
namespace {

constexpr std::size_t kMaximumTranscriptBytes = 320;
constexpr std::size_t kMaximumAnswerBytes = 640;
constexpr std::size_t kMaximumErrorBytes = 120;
constexpr std::size_t kMaximumProgressBytes = 80;
constexpr std::size_t kMaximumWifiStatusBytes = 20;

lv_obj_t *status_label = nullptr;
lv_obj_t *hint_label = nullptr;
lv_obj_t *content_label = nullptr;
lv_obj_t *level_bar = nullptr;
lv_obj_t *activity_spinner = nullptr;
lv_obj_t *wifi_status_label = nullptr;
lv_obj_t *battery_status_label = nullptr;
lv_obj_t *image_overlay = nullptr;
lv_obj_t *passkey_overlay = nullptr;
lv_obj_t *passkey_label = nullptr;

image::Rgb565Frame image_frame;
lv_image_dsc_t image_descriptor{};

std::array<char, kMaximumAnswerBytes + 1> content_buffer{};
std::array<char, kMaximumProgressBytes + 1> hint_buffer{};
std::array<char, 7> passkey_buffer{};
std::array<char, kMaximumWifiStatusBytes + 1> wifi_status_buffer{};
std::array<char, 12> battery_status_buffer{};

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

const char *status(InteractionState state) {
    if (state == InteractionState::booting) {
        return "CHAT ESP";
    }
    return state_name(state);
}

void copy_bounded(
    std::string_view source,
    char *destination,
    std::size_t destination_size,
    std::size_t maximum_bytes) {
    if (destination == nullptr || destination_size == 0) {
        return;
    }
    std::fill_n(destination, destination_size, '\0');

    const std::size_t limit =
        std::min({source.size(), maximum_bytes, destination_size - 1});
    std::size_t source_index = 0;
    std::size_t destination_index = 0;
    while (source_index < limit) {
        const auto first = static_cast<unsigned char>(source[source_index]);
        if (first < 0x80) {
            destination[destination_index++] =
                first < 0x20 && first != '\n' ? ' ' : source[source_index];
            ++source_index;
            continue;
        }

        std::size_t sequence_size = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            sequence_size = 2;
        } else if (first >= 0xe0 && first <= 0xef) {
            sequence_size = 3;
        } else if (first >= 0xf0 && first <= 0xf4) {
            sequence_size = 4;
        }

        bool valid = sequence_size != 0 &&
            source_index + sequence_size <= limit &&
            destination_index + sequence_size < destination_size;
        for (std::size_t index = 1; valid && index < sequence_size; ++index) {
            const auto next =
                static_cast<unsigned char>(source[source_index + index]);
            valid = (next & 0xc0) == 0x80;
        }
        if (valid && sequence_size == 3) {
            const auto second =
                static_cast<unsigned char>(source[source_index + 1]);
            valid = (first != 0xe0 || second >= 0xa0) &&
                (first != 0xed || second <= 0x9f);
        } else if (valid && sequence_size == 4) {
            const auto second =
                static_cast<unsigned char>(source[source_index + 1]);
            valid = (first != 0xf0 || second >= 0x90) &&
                (first != 0xf4 || second <= 0x8f);
        }

        if (!valid) {
            destination[destination_index++] = '?';
            ++source_index;
            continue;
        }
        for (std::size_t index = 0; index < sequence_size; ++index) {
            destination[destination_index++] = source[source_index + index];
        }
        source_index += sequence_size;
    }
    destination[destination_index] = '\0';
}

void set_static_text(lv_obj_t *label, const char *text) {
    if (label != nullptr) {
        lv_label_set_text_static(label, text);
    }
}

void set_content(std::string_view text, std::size_t maximum_bytes) {
    copy_bounded(
        text, content_buffer.data(), content_buffer.size(), maximum_bytes);
    set_static_text(content_label, content_buffer.data());
}

void set_hint(std::string_view text) {
    copy_bounded(
        text, hint_buffer.data(), hint_buffer.size(), kMaximumProgressBytes);
    set_static_text(hint_label, hint_buffer.data());
}

void show_activity(bool visible) {
    if (activity_spinner == nullptr) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(activity_spinner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(activity_spinner, LV_OBJ_FLAG_HIDDEN);
    }
}

void hide_passkey() {
    if (passkey_overlay != nullptr) {
        lv_obj_add_flag(passkey_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    std::fill(passkey_buffer.begin(), passkey_buffer.end(), '\0');
}

void prepare_voice_view() {
    hide_fullscreen_image();
    hide_passkey();
    if (level_bar != nullptr) {
        lv_obj_add_flag(level_bar, LV_OBJ_FLAG_HIDDEN);
    }
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
    lv_obj_align(status_label, LV_ALIGN_TOP_LEFT, 16, 44);

    hint_label = lv_label_create(screen);
    lv_obj_set_width(hint_label, 306);
    lv_obj_set_style_text_color(hint_label, lv_color_hex(0x777777), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(hint_label, 1, LV_PART_MAIN);
    lv_obj_align(hint_label, LV_ALIGN_TOP_LEFT, 16, 84);

    content_label = lv_label_create(screen);
    lv_obj_set_size(content_label, 336, 270);
    lv_label_set_long_mode(content_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(
        content_label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        content_label, &chatesp_font_18, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(content_label, 7, LV_PART_MAIN);
    lv_obj_set_style_text_align(
        content_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_align(content_label, LV_ALIGN_TOP_LEFT, 16, 126);
    set_static_text(content_label, content_buffer.data());

    level_bar = lv_bar_create(screen);
    lv_obj_set_size(level_bar, 336, 3);
    lv_obj_align(level_bar, LV_ALIGN_BOTTOM_LEFT, 16, -48);
    lv_obj_set_style_bg_color(
        level_bar, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(level_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        level_bar, lv_color_hex(0xffffff), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(level_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_range(level_bar, 0, 100);
    lv_bar_set_value(level_bar, 0, LV_ANIM_OFF);
    lv_obj_add_flag(level_bar, LV_OBJ_FLAG_HIDDEN);

#if LVGL_VERSION_MAJOR >= 9
    activity_spinner = lv_spinner_create(screen);
#else
    activity_spinner = lv_spinner_create(screen, 900, 70);
#endif
    lv_obj_set_size(activity_spinner, 18, 18);
    lv_obj_align(activity_spinner, LV_ALIGN_TOP_RIGHT, -17, 81);
    lv_obj_set_style_arc_width(activity_spinner, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_width(activity_spinner, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(
        activity_spinner, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_style_arc_color(
        activity_spinner, lv_color_hex(0xffffff), LV_PART_INDICATOR);
    lv_obj_add_flag(activity_spinner, LV_OBJ_FLAG_HIDDEN);

    wifi_status_label = lv_label_create(screen);
    lv_obj_set_width(wifi_status_label, 220);
    lv_obj_set_style_text_color(
        wifi_status_label, lv_color_hex(0x777777), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        wifi_status_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(
        wifi_status_label, 1, LV_PART_MAIN);
    lv_obj_set_style_text_align(
        wifi_status_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_align(wifi_status_label, LV_ALIGN_BOTTOM_LEFT, 30, -20);

    battery_status_label = lv_label_create(screen);
    lv_obj_set_width(battery_status_label, 80);
    lv_obj_set_style_text_color(
        battery_status_label, lv_color_hex(0x777777), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        battery_status_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(
        battery_status_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align(battery_status_label, LV_ALIGN_BOTTOM_RIGHT, -30, -20);
    show_footer(WifiIndicator::off, false, 0);

    image_overlay = lv_image_create(screen);
    lv_obj_set_size(
        image_overlay, image::kDisplayWidth, image::kDisplayHeight);
    lv_obj_center(image_overlay);
    lv_obj_add_flag(image_overlay, LV_OBJ_FLAG_HIDDEN);

    passkey_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(passkey_overlay);
    lv_obj_set_size(passkey_overlay, 368, 448);
    lv_obj_set_style_bg_color(
        passkey_overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(passkey_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(passkey_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(passkey_overlay);

    lv_obj_t *passkey_title = lv_label_create(passkey_overlay);
    set_static_text(passkey_title, "PAIRING CODE");
    lv_obj_set_style_text_color(
        passkey_title, lv_color_hex(0x777777), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        passkey_title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(passkey_title, 1, LV_PART_MAIN);
    lv_obj_align(passkey_title, LV_ALIGN_CENTER, 0, -42);

    passkey_label = lv_label_create(passkey_overlay);
    set_static_text(passkey_label, passkey_buffer.data());
    lv_obj_set_style_text_color(
        passkey_label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        passkey_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(passkey_label, 5, LV_PART_MAIN);
    lv_obj_align(passkey_label, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *passkey_hint = lv_label_create(passkey_overlay);
    set_static_text(passkey_hint, "ENTER THIS CODE ON IPHONE");
    lv_obj_set_style_text_color(
        passkey_hint, lv_color_hex(0x777777), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        passkey_hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(passkey_hint, LV_ALIGN_CENTER, 0, 44);
    hide_passkey();
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
    if (state != InteractionState::idle && state != InteractionState::booting) {
        hide_passkey();
    }
    if (state != InteractionState::idle) {
        hide_fullscreen_image();
    }
    if (state == InteractionState::booting ||
        state == InteractionState::recording ||
        state == InteractionState::sleep_pending) {
        set_content({}, kMaximumAnswerBytes);
    }
    set_static_text(status_label, status(state));
    set_static_text(hint_label, hint(state));
    show_activity(
        false);
    if (level_bar != nullptr) {
        if (state == InteractionState::recording) {
            lv_bar_set_value(level_bar, 0, LV_ANIM_OFF);
            lv_obj_clear_flag(level_bar, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(level_bar, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void show_recording_level(std::uint8_t percent) {
    if (level_bar == nullptr) {
        return;
    }
    hide_passkey();
    lv_bar_set_value(
        level_bar, std::min<int>(percent, 100), LV_ANIM_ON);
}

void show_transcript(std::string_view transcript) {
    prepare_voice_view();
    show_activity(false);
    set_static_text(status_label, "YOU");
    set_static_text(hint_label, "TRANSCRIPT");
    set_content(transcript, kMaximumTranscriptBytes);
}

void show_answer_stream(std::string_view answer) {
    prepare_voice_view();
    show_activity(false);
    set_static_text(status_label, "ANSWER");
    set_static_text(hint_label, "RECEIVING");
    set_content(answer, kMaximumAnswerBytes);
}

void show_answer(std::string_view answer) {
    prepare_voice_view();
    show_activity(false);
    set_static_text(status_label, "ANSWER");
    set_static_text(hint_label, "PREPARING SPEECH");
    set_content(answer, kMaximumAnswerBytes);
}

void show_answer_notice(std::string_view answer, std::string_view notice) {
    prepare_voice_view();
    show_activity(false);
    set_static_text(status_label, "ANSWER");
    set_hint(notice);
    set_content(answer, kMaximumAnswerBytes);
}

void show_error(std::string_view error) {
    prepare_voice_view();
    show_activity(false);
    set_static_text(status_label, "TRY AGAIN");
    set_static_text(hint_label, "THE REQUEST DID NOT FINISH");
    set_content(error, kMaximumErrorBytes);
}

void show_wifi_progress(std::string_view detail) {
    prepare_voice_view();
    show_activity(false);
    set_static_text(status_label, "WI-FI");
    set_hint(detail);
}

void show_model_progress(std::string_view detail) {
    prepare_voice_view();
    show_activity(false);
    set_static_text(status_label, "THINKING");
    set_hint(detail);
}

void show_ble_passkey(std::uint32_t passkey, bool visible) {
    if (passkey_overlay == nullptr || passkey_label == nullptr) {
        return;
    }
    if (!visible || passkey > 999999) {
        hide_passkey();
        return;
    }

    std::snprintf(
        passkey_buffer.data(),
        passkey_buffer.size(),
        "%06lu",
        static_cast<unsigned long>(passkey));
    set_static_text(passkey_label, passkey_buffer.data());
    show_activity(false);
    if (level_bar != nullptr) {
        lv_obj_add_flag(level_bar, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_move_foreground(passkey_overlay);
    lv_obj_clear_flag(passkey_overlay, LV_OBJ_FLAG_HIDDEN);
}

void show_footer(
    WifiIndicator wifi, bool battery_available,
    std::uint8_t battery_percent) {
    const char *wifi_text = LV_SYMBOL_WIFI " OFF";
    switch (wifi) {
        case WifiIndicator::setup:
            wifi_text = LV_SYMBOL_SETTINGS " SETUP";
            break;
        case WifiIndicator::off:
            wifi_text = LV_SYMBOL_WIFI " OFF";
            break;
        case WifiIndicator::connecting:
            wifi_text = LV_SYMBOL_WIFI " CONNECTING";
            break;
        case WifiIndicator::online:
            wifi_text = LV_SYMBOL_WIFI " ONLINE";
            break;
        case WifiIndicator::failed:
            wifi_text = LV_SYMBOL_WARNING " RETRY";
            break;
    }
    copy_bounded(
        wifi_text,
        wifi_status_buffer.data(),
        wifi_status_buffer.size(),
        kMaximumWifiStatusBytes);
    set_static_text(wifi_status_label, wifi_status_buffer.data());

    const char *battery_icon = LV_SYMBOL_BATTERY_EMPTY;
    if (battery_available && battery_percent >= 80) {
        battery_icon = LV_SYMBOL_BATTERY_FULL;
    } else if (battery_available && battery_percent >= 60) {
        battery_icon = LV_SYMBOL_BATTERY_3;
    } else if (battery_available && battery_percent >= 35) {
        battery_icon = LV_SYMBOL_BATTERY_2;
    } else if (battery_available && battery_percent >= 10) {
        battery_icon = LV_SYMBOL_BATTERY_1;
    }
    if (battery_available && battery_percent <= 100) {
        std::snprintf(
            battery_status_buffer.data(),
            battery_status_buffer.size(),
            "%s %u%%",
            battery_icon,
            static_cast<unsigned>(battery_percent));
    } else {
        std::snprintf(
            battery_status_buffer.data(),
            battery_status_buffer.size(),
            "%s --%%",
            battery_icon);
    }
    set_static_text(battery_status_label, battery_status_buffer.data());
}

bool show_fullscreen_image(image::Rgb565Frame &&frame) {
    if (image_overlay == nullptr || !frame.available()) {
        return false;
    }

    hide_fullscreen_image();
    image_frame = std::move(frame);
    image_descriptor = {};
    image_descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
    image_descriptor.header.w = image::kDisplayWidth;
    image_descriptor.header.h = image::kDisplayHeight;
    image_descriptor.header.stride = image::kDisplayWidth * sizeof(std::uint16_t);
    image_descriptor.data_size = image_frame.size_bytes();
    image_descriptor.data = reinterpret_cast<const std::uint8_t *>(
        image_frame.data());

    lv_image_set_src(image_overlay, &image_descriptor);
    lv_obj_center(image_overlay);
    lv_obj_clear_flag(image_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(image_overlay);
    if (passkey_overlay != nullptr &&
        !lv_obj_has_flag(passkey_overlay, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(passkey_overlay);
    }
    lv_obj_invalidate(image_overlay);
    return true;
}

void hide_fullscreen_image() {
    if (image_overlay == nullptr || !image_frame.available()) {
        return;
    }
    lv_obj_add_flag(image_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(image_overlay, nullptr);
    lv_obj_invalidate(image_overlay);
    lv_refr_now(nullptr);
    image_descriptor = {};
    image_frame.reset();
}

esp_err_t sleep() {
    if (!bsp_display_lock(25)) {
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t result = bsp_display_backlight_off();
    bsp_display_unlock();
    return result;
}

esp_err_t wake(InteractionState state) {
    // A held wake must reach microphone capture without a long display wait.
    if (!bsp_display_lock(25)) {
        return ESP_ERR_TIMEOUT;
    }
    hide_fullscreen_image();
    set_content({}, kMaximumAnswerBytes);
    show_state(state);
    lv_refr_now(nullptr);
    bsp_display_unlock();
    const esp_err_t wake_error = bsp_display_brightness_set(65);
    if (wake_error != ESP_OK) {
        return wake_error;
    }
    // Redraw once after panel-on. The CO5300 can acknowledge an off-screen
    // flush without making it visible until the next transfer.
    if (!bsp_display_lock(25)) {
        return ESP_ERR_TIMEOUT;
    }
    lv_obj_invalidate(active_screen());
    lv_refr_now(nullptr);
    bsp_display_unlock();
    return bsp_display_brightness_set(65);
}

esp_err_t reassert_panel() {
    return bsp_display_brightness_set(65);
}

}  // namespace chatesp::ui
