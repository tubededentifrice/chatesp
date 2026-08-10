#include "ui.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <utility>

#include "bsp/esp-bsp.h"
#include "chatesp/device_preferences.hpp"
#include "chatesp/quick_controls.hpp"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "recording_spectrum_view.hpp"

LV_FONT_DECLARE(chatesp_font_18);
LV_FONT_DECLARE(chatesp_clock_font_180);

#ifndef CHATESP_DEVELOPMENT_MODE
#define CHATESP_DEVELOPMENT_MODE 0
#endif

namespace chatesp::ui {
namespace {

constexpr std::size_t kMaximumTranscriptBytes = 320;
constexpr std::size_t kMaximumAnswerBytes = 640;
constexpr std::size_t kMaximumErrorBytes = 120;
constexpr std::size_t kMaximumProgressBytes = 80;
constexpr std::size_t kMaximumWifiStatusBytes = 20;
constexpr std::int32_t kControlsPanelHeight = 280;
constexpr std::int32_t kControlsPanelShownY = -12;
constexpr std::int32_t kControlsHandleWidth = 44;
constexpr std::int32_t kControlsHandleHeight = 4;
constexpr std::int32_t kControlsEdgeHandleY = 8;
constexpr std::int32_t kControlsPanelHandleBottomInset = 12;
constexpr std::int32_t kControlsPanelHandleY =
    kControlsPanelHeight - kControlsPanelHandleBottomInset -
    kControlsHandleHeight;
constexpr std::int32_t kControlsPanelHiddenY =
    kControlsEdgeHandleY - kControlsPanelHandleY;
constexpr std::uint32_t kControlsOpenAnimationMs = 180;
constexpr std::uint32_t kControlsCloseAnimationMs = 140;
constexpr std::int32_t kControlSliderLayoutWidth = 320;
constexpr std::int32_t kControlSliderLayoutHeight = 44;
constexpr std::int32_t kControlSliderTouchWidth = 352;
constexpr std::int32_t kControlSliderTouchHeight = 64;
constexpr std::int32_t kControlSliderTrackInset = 16;
constexpr std::int32_t kControlSliderTrackWidth =
    kControlSliderLayoutWidth - 2 * kControlSliderTrackInset;
constexpr std::int32_t kControlSliderTrackTouchInset =
    (kControlSliderTouchWidth - kControlSliderTrackWidth) / 2;
constexpr std::int32_t kControlSliderTouchOffsetY =
    (kControlSliderLayoutHeight - kControlSliderTouchHeight) / 2;
constexpr std::int32_t kControlSliderTrackHeight = 12;
constexpr std::int32_t kControlSliderKnobSize = 32;
static_assert(
    kControlSliderTrackTouchInset ==
    (kControlSliderTouchWidth - kControlSliderLayoutWidth) / 2 +
        kControlSliderTrackInset);
static_assert(
    kControlSliderTouchOffsetY + kControlSliderTouchHeight / 2 ==
    kControlSliderLayoutHeight / 2);
static_assert(
    kControlsPanelHiddenY + kControlsPanelHandleY ==
    kControlsEdgeHandleY);

enum class ControlKind : std::uint8_t {
    brightness,
    volume,
};

enum class ContentKind : std::uint8_t {
    none,
    transcript,
    answer,
    error,
};

struct ControlSlider {
    ControlKind kind = ControlKind::brightness;
    lv_obj_t *touch_target = nullptr;
    lv_obj_t *indicator = nullptr;
    lv_obj_t *knob = nullptr;
    lv_obj_t *value_label = nullptr;
    std::array<char, 5> value_buffer{};
    std::uint8_t minimum_percent = 0;
    std::uint8_t value_percent = 0;
};
constexpr std::size_t kMaximumClockPathPointCount = 1'632;
constexpr std::uint32_t kClockPathRefreshMs = 16;
constexpr std::int32_t kClockWidth = image::kDisplayHeight;
constexpr std::int32_t kClockHeight = image::kDisplayWidth;
constexpr std::int32_t kClockRight = kClockWidth - 1;
constexpr std::int32_t kClockBottom = kClockHeight - 1;
constexpr double kPi = 3.14159265358979323846;
static_assert(
    kMaximumClockPathPointCount >=
    2U * static_cast<std::size_t>(kClockRight + kClockBottom));

lv_obj_t *status_label = nullptr;
lv_obj_t *hint_label = nullptr;
lv_obj_t *content_viewport = nullptr;
lv_obj_t *content_label = nullptr;
lv_obj_t *activity_spinner = nullptr;
lv_obj_t *wifi_status_label = nullptr;
lv_obj_t *battery_icon_label = nullptr;
lv_obj_t *battery_charge_label = nullptr;
lv_obj_t *battery_status_label = nullptr;
lv_obj_t *image_overlay = nullptr;
lv_obj_t *plot_overlay = nullptr;
lv_obj_t *plot_chart = nullptr;
lv_obj_t *plot_title_label = nullptr;
lv_obj_t *plot_x_range_label = nullptr;
lv_obj_t *plot_y_range_label = nullptr;
lv_chart_series_t *plot_series = nullptr;
lv_obj_t *passkey_overlay = nullptr;
lv_obj_t *passkey_label = nullptr;
lv_obj_t *sleep_overlay = nullptr;
bool touch_available = false;
lv_obj_t *controls_edge_target = nullptr;
lv_obj_t *controls_edge_handle = nullptr;
lv_obj_t *controls_backdrop = nullptr;
lv_obj_t *controls_panel = nullptr;
lv_timer_t *controls_timer = nullptr;
ControlSlider brightness_control;
ControlSlider volume_control;
ControlSlider *active_control = nullptr;
lv_display_t *display_handle = nullptr;
lv_obj_t *clock_root = nullptr;
lv_obj_t *clock_time_label = nullptr;
lv_timer_t *clock_path_timer = nullptr;

lv_point_precise_t *clock_path_points = nullptr;
std::uint16_t clock_path_point_count = 0;
ClockPathSpan shown_clock_path{};

image::Rgb565Frame image_frame;
lv_image_dsc_t image_descriptor{};

std::array<char, kMaximumAnswerBytes + 1> content_buffer{};
ContentKind shown_content_kind = ContentKind::none;
std::array<char, kMaximumProgressBytes + 1> hint_buffer{};
std::array<char, 7> passkey_buffer{};
std::array<char, kMaximumWifiStatusBytes + 1> wifi_status_buffer{};
std::array<char, 5> battery_status_buffer{};
std::array<char, 6> clock_time_buffer{'-', '-', ':', '-', '-', '\0'};
QuickControlsGesture controls_gesture;
QuickControlsCallback controls_callback = nullptr;
void *controls_callback_context = nullptr;
std::uint8_t controls_brightness_percent =
    runtime::DevicePreferences::default_brightness_percent;
std::uint8_t controls_volume_percent =
    runtime::DevicePreferences::default_volume_percent;
bool controls_state_allowed = false;
bool controls_close_animation_active = false;
bool controls_drag_visible = false;
std::int32_t controls_drag_panel_start_y = kControlsPanelHiddenY;
bool passkey_visible = false;
bool clock_mode = false;
ClockStyle clock_style{};
std::uint8_t shown_clock_hour = 0xff;
std::uint8_t shown_clock_minute = 0xff;
std::uint8_t shown_clock_second = 0xff;
std::uint16_t shown_clock_millisecond = 0;
std::uint32_t shown_clock_tick = 0;
bool shown_clock_available = false;
bool clock_face_initialized = false;
std::array<char, agent::Limits::max_plot_title_bytes + 1> plot_title_buffer{};
std::array<char, 48> plot_x_range_buffer{};
std::array<char, 48> plot_y_range_buffer{};
std::array<std::int32_t, agent::Limits::max_plot_points> plot_x_values{};
std::array<std::int32_t, agent::Limits::max_plot_points> plot_y_values{};

void set_static_text(lv_obj_t *label, const char *text);
void update_controls_handle();

void hide_fullscreen_plot() {
    if (plot_overlay == nullptr) {
        return;
    }
    lv_obj_add_flag(plot_overlay, LV_OBJ_FLAG_HIDDEN);
    plot_x_values.fill(0);
    plot_y_values.fill(0);
    plot_title_buffer.fill('\0');
    plot_x_range_buffer.fill('\0');
    plot_y_range_buffer.fill('\0');
    set_static_text(plot_title_label, plot_title_buffer.data());
    set_static_text(plot_x_range_label, plot_x_range_buffer.data());
    set_static_text(plot_y_range_label, plot_y_range_buffer.data());
    lv_obj_invalidate(plot_overlay);
}

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
            return "RUNNING A TOOL";
        case InteractionState::speaking:
            return "PLAYING ANSWER";
        case InteractionState::error:
            return "READ THE ERROR DETAILS";
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

void stop_content_scroll() {
    lv_indev_t *input = lv_indev_get_next(nullptr);
    while (input != nullptr) {
        lv_indev_t *next = lv_indev_get_next(input);
        if (lv_indev_get_scroll_obj(input) == content_viewport) {
            lv_indev_reset(input, content_viewport);
        }
        input = next;
    }
}

void set_content(
    std::string_view text,
    std::size_t maximum_bytes,
    ContentKind kind) {
    const bool reset_scroll = shown_content_kind != kind;
    if (reset_scroll) {
        stop_content_scroll();
    }
    copy_bounded(
        text, content_buffer.data(), content_buffer.size(), maximum_bytes);
    set_static_text(content_label, content_buffer.data());
    shown_content_kind = kind;
    if (reset_scroll && content_viewport != nullptr) {
        lv_obj_scroll_to_y(content_viewport, 0, LV_ANIM_OFF);
    }
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

bool controls_allowed_for_state(InteractionState state) {
    return state != InteractionState::booting &&
        state != InteractionState::recording &&
        state != InteractionState::sleep_pending;
}

void set_hidden(lv_obj_t *object, bool hidden) {
    if (object == nullptr) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

void layout_overlays(std::int32_t width, std::int32_t height) {
    if (controls_backdrop != nullptr) {
        lv_obj_set_size(controls_backdrop, width, height);
    }
    if (controls_panel != nullptr) {
        lv_obj_set_width(controls_panel, width);
    }
    if (controls_edge_target != nullptr) {
        lv_obj_set_width(controls_edge_target, width);
        lv_obj_align(controls_edge_target, LV_ALIGN_TOP_MID, 0, 0);
    }
    if (passkey_overlay != nullptr) {
        lv_obj_set_size(passkey_overlay, width, height);
        lv_obj_center(passkey_overlay);
    }
}

void apply_display_orientation() {
    if (display_handle == nullptr) {
        return;
    }
    const AppMode mode = clock_mode ? AppMode::clock : AppMode::chat;
    if (display_orientation_for(mode, passkey_visible) ==
        DisplayOrientation::clock) {
#if LVGL_VERSION_MAJOR >= 9
        bsp_display_rotate(display_handle, LV_DISPLAY_ROTATION_270);
#else
        bsp_display_rotate(display_handle, LV_DISP_ROT_270);
#endif
        layout_overlays(kClockWidth, kClockHeight);
        return;
    }
#if LVGL_VERSION_MAJOR >= 9
    bsp_display_rotate(display_handle, LV_DISPLAY_ROTATION_0);
#else
    bsp_display_rotate(display_handle, LV_DISP_ROT_NONE);
#endif
    layout_overlays(image::kDisplayWidth, image::kDisplayHeight);
}

void rounded_clock_point(double distance, double &x, double &y) {
    constexpr double left = 0.0;
    constexpr double top = 0.0;
    constexpr double right = kClockRight;
    constexpr double bottom = kClockBottom;
    const double radius = clock_style.corner_radius_px;
    const double half_top = (right - left - 2.0 * radius) / 2.0;
    const double vertical = bottom - top - 2.0 * radius;
    const double horizontal = right - left - 2.0 * radius;
    const double quarter_arc = kPi * radius / 2.0;

    if (distance < half_top) {
        x = (left + right) / 2.0 + distance;
        y = top;
        return;
    }
    distance -= half_top;
    if (distance < quarter_arc) {
        const double angle = -kPi / 2.0 + distance / radius;
        x = right - radius + radius * std::cos(angle);
        y = top + radius + radius * std::sin(angle);
        return;
    }
    distance -= quarter_arc;
    if (distance < vertical) {
        x = right;
        y = top + radius + distance;
        return;
    }
    distance -= vertical;
    if (distance < quarter_arc) {
        const double angle = distance / radius;
        x = right - radius + radius * std::cos(angle);
        y = bottom - radius + radius * std::sin(angle);
        return;
    }
    distance -= quarter_arc;
    if (distance < horizontal) {
        x = right - radius - distance;
        y = bottom;
        return;
    }
    distance -= horizontal;
    if (distance < quarter_arc) {
        const double angle = kPi / 2.0 + distance / radius;
        x = left + radius + radius * std::cos(angle);
        y = bottom - radius + radius * std::sin(angle);
        return;
    }
    distance -= quarter_arc;
    if (distance < vertical) {
        x = left;
        y = bottom - radius - distance;
        return;
    }
    distance -= vertical;
    if (distance < quarter_arc) {
        const double angle = kPi + distance / radius;
        x = left + radius + radius * std::cos(angle);
        y = top + radius + radius * std::sin(angle);
        return;
    }
    distance -= quarter_arc;
    x = left + radius + std::min(distance, half_top);
    y = top;
}

void layout_clock_path() {
    if (clock_path_points == nullptr) {
        clock_path_points = static_cast<lv_point_precise_t *>(
            heap_caps_calloc(
                kMaximumClockPathPointCount, sizeof(lv_point_precise_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (clock_path_points == nullptr) {
            clock_path_point_count = 0;
            return;
        }
    }
    const double radius = clock_style.corner_radius_px;
    const double horizontal =
        static_cast<double>(kClockRight) - 2.0 * radius;
    const double vertical =
        static_cast<double>(kClockBottom) - 2.0 * radius;
    const double perimeter =
        2.0 * horizontal + 2.0 * vertical + 2.0 * kPi * radius;
    const std::size_t sample_count = static_cast<std::size_t>(
        std::ceil(perimeter * 2.0));
    clock_path_point_count = 0;
    for (std::size_t index = 0;
         index < sample_count &&
         clock_path_point_count < kMaximumClockPathPointCount;
         ++index) {
        double x = 0.0;
        double y = 0.0;
        rounded_clock_point(
            static_cast<double>(index) * perimeter /
                static_cast<double>(sample_count),
            x, y);
        const lv_point_precise_t candidate{
            static_cast<lv_value_precise_t>(std::lround(x)),
            static_cast<lv_value_precise_t>(std::lround(y)),
        };
        if (clock_path_point_count != 0) {
            const lv_point_precise_t &previous =
                clock_path_points[clock_path_point_count - 1];
            if (candidate.x == previous.x && candidate.y == previous.y) {
                continue;
            }
        }
        clock_path_points[clock_path_point_count++] = candidate;
    }
}

ClockPathSpan current_clock_path_span() {
    if (!shown_clock_available || clock_path_point_count == 0) {
        return {};
    }
    const std::uint32_t elapsed_ms =
        static_cast<std::uint32_t>(shown_clock_second) * 1'000U +
        shown_clock_millisecond + lv_tick_elaps(shown_clock_tick);
    const std::uint8_t minute = static_cast<std::uint8_t>(
        (static_cast<std::uint32_t>(shown_clock_minute) +
         elapsed_ms / 60'000U) % 60U);
    return clock_path_span(
        minute, elapsed_ms % 60'000U, clock_path_point_count);
}

bool same_clock_path(ClockPathSpan left, ClockPathSpan right) {
    return left.first == right.first && left.count == right.count;
}

void clock_path_timer_callback(lv_timer_t *) {
    if (!clock_mode || clock_root == nullptr || !shown_clock_available) {
        return;
    }
    const ClockPathSpan next = current_clock_path_span();
    if (same_clock_path(next, shown_clock_path)) {
        return;
    }
    shown_clock_path = next;
    lv_obj_invalidate(clock_root);
}

void draw_clock(lv_event_t *event) {
    if (lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_obj_t *object = lv_event_get_current_target_obj(event);
    if (layer == nullptr || object == nullptr) {
        return;
    }
    if (shown_clock_path.count == 0 || clock_path_point_count == 0) {
        return;
    }

    lv_draw_line_dsc_t descriptor;
    lv_draw_line_dsc_init(&descriptor);
    descriptor.base.layer = layer;
    descriptor.color = lv_color_hex(clock_style.seconds_rgb);
    descriptor.opa = LV_OPA_COVER;
    descriptor.width = 1;
    descriptor.round_start = 1;
    descriptor.round_end = 1;
    if (shown_clock_path.count == 1) {
        lv_area_t root;
        lv_obj_get_coords(object, &root);
        const lv_point_precise_t point =
            clock_path_points[shown_clock_path.first];
        const lv_area_t pixel{
            root.x1 + static_cast<std::int32_t>(point.x),
            root.y1 + static_cast<std::int32_t>(point.y),
            root.x1 + static_cast<std::int32_t>(point.x),
            root.y1 + static_cast<std::int32_t>(point.y),
        };
        lv_draw_rect_dsc_t pixel_descriptor;
        lv_draw_rect_dsc_init(&pixel_descriptor);
        pixel_descriptor.base.layer = layer;
        pixel_descriptor.bg_color = descriptor.color;
        pixel_descriptor.bg_opa = descriptor.opa;
        lv_draw_rect(layer, &pixel_descriptor, &pixel);
        return;
    } else {
        descriptor.points = clock_path_points + shown_clock_path.first;
        descriptor.point_cnt = shown_clock_path.count;
    }
    lv_draw_line(layer, &descriptor);
}

void apply_clock_style(bool layout_path) {
    if (clock_root == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(
        clock_root, lv_color_hex(clock_style.background_rgb), LV_PART_MAIN);
    if (clock_time_label != nullptr) {
        lv_obj_set_style_text_color(
            clock_time_label, lv_color_hex(clock_style.time_rgb),
            LV_PART_MAIN);
    }
    if (layout_path) {
        layout_clock_path();
    } else {
        clock_path_point_count = 0;
    }
    shown_clock_path = current_clock_path_span();
    lv_obj_invalidate(clock_root);
}

void create_clock_face(lv_obj_t *screen) {
    clock_root = lv_obj_create(screen);
    lv_obj_remove_style_all(clock_root);
    lv_obj_set_size(clock_root, kClockWidth, kClockHeight);
    lv_obj_set_pos(clock_root, 0, 0);
    lv_obj_set_style_bg_opa(clock_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(clock_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(clock_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(clock_root, draw_clock, LV_EVENT_DRAW_MAIN, nullptr);

    clock_time_label = lv_label_create(clock_root);
    lv_obj_remove_style_all(clock_time_label);
    lv_label_set_text_static(clock_time_label, clock_time_buffer.data());
    lv_obj_set_style_text_font(
        clock_time_label, &chatesp_clock_font_180, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(clock_time_label, -10, LV_PART_MAIN);
    lv_obj_set_style_text_opa(clock_time_label, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_center(clock_time_label);

    // The rounded Clock path uses double-precision trigonometry. It is not
    // needed for voice wake. Build it only when Clock first opens.
    apply_clock_style(false);
    clock_path_timer = lv_timer_create(
        clock_path_timer_callback, kClockPathRefreshMs, nullptr);
    lv_timer_pause(clock_path_timer);
    set_hidden(clock_root, true);
}

void bring_clock_overlays_forward() {
    if (passkey_visible && passkey_overlay != nullptr) {
        lv_obj_move_foreground(passkey_overlay);
        return;
    }
    update_controls_handle();
    if (controls_gesture.open()) {
        lv_obj_move_foreground(controls_backdrop);
        lv_obj_move_foreground(controls_panel);
    } else if (controls_edge_target != nullptr &&
               !lv_obj_has_flag(controls_edge_target, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(controls_edge_target);
    }
}

void update_controls_handle() {
    const bool visible = controls_callback != nullptr &&
        controls_gesture.allowed() && !controls_gesture.open() &&
        !controls_close_animation_active && !passkey_visible;
    set_hidden(controls_edge_target, !visible);
}

void format_percent(ControlSlider &control) {
    std::snprintf(
        control.value_buffer.data(), control.value_buffer.size(), "%u%%",
        static_cast<unsigned>(control.value_percent));
    set_static_text(control.value_label, control.value_buffer.data());
}

void set_control_value(ControlSlider &control, std::uint8_t percent) {
    control.value_percent = percent;
    const std::int32_t range = 100 - control.minimum_percent;
    const std::int32_t relative = range == 0
        ? 0
        : (static_cast<std::int32_t>(percent) - control.minimum_percent) *
            kControlSliderTrackWidth / range;
    if (control.indicator != nullptr) {
        lv_obj_set_width(control.indicator, std::max<std::int32_t>(1, relative));
    }
    if (control.knob != nullptr) {
        lv_obj_set_x(
            control.knob,
            kControlSliderTrackTouchInset + relative -
                kControlSliderKnobSize / 2);
    }
    format_percent(control);
}

void sync_controls_values(
    std::uint8_t brightness_percent, std::uint8_t volume_percent) {
    controls_brightness_percent = brightness_percent;
    controls_volume_percent = volume_percent;
    set_control_value(brightness_control, brightness_percent);
    set_control_value(volume_control, volume_percent);
}

void dispatch_controls_update(
    bool brightness_changed, bool volume_changed, bool commit) {
    controls_gesture.note_activity(lv_tick_get());
    if (controls_callback == nullptr) {
        return;
    }
    const QuickControlsUpdate update{
        controls_brightness_percent,
        controls_volume_percent,
        brightness_changed,
        volume_changed,
        commit,
    };
    controls_callback(update, controls_callback_context);
}

void controls_panel_y_animation(void *object, std::int32_t value) {
    lv_obj_set_y(static_cast<lv_obj_t *>(object), value);
}

void finish_controls_close_animation(lv_anim_t *) {
    controls_close_animation_active = false;
    controls_drag_visible = false;
    set_hidden(controls_panel, true);
    set_hidden(controls_backdrop, true);
    update_controls_handle();
}

std::uint32_t controls_animation_duration(
    std::int32_t from_y,
    std::int32_t to_y,
    std::uint32_t full_duration_ms) {
    const std::int32_t distance = std::abs(to_y - from_y);
    const std::int32_t travel =
        kControlsPanelShownY - kControlsPanelHiddenY;
    if (distance == 0 || travel <= 0) {
        return 0;
    }
    return std::max<std::uint32_t>(
        48, full_duration_ms * distance / travel);
}

void show_controls_layer() {
    set_hidden(controls_backdrop, false);
    set_hidden(controls_panel, false);
    lv_obj_move_foreground(controls_backdrop);
    lv_obj_move_foreground(controls_panel);
}

void settle_controls_open() {
    if (controls_panel == nullptr) {
        return;
    }
    controls_close_animation_active = false;
    controls_drag_visible = false;
    controls_gesture.set_open(true, lv_tick_get());
    set_hidden(controls_edge_target, true);
    show_controls_layer();
    lv_anim_delete(controls_panel, controls_panel_y_animation);
    const std::int32_t current_y = lv_obj_get_y(controls_panel);
    const std::uint32_t duration = controls_animation_duration(
        current_y, kControlsPanelShownY, kControlsOpenAnimationMs);
    if (duration == 0) {
        lv_obj_set_y(controls_panel, kControlsPanelShownY);
        return;
    }

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, controls_panel);
    lv_anim_set_values(&animation, current_y, kControlsPanelShownY);
    lv_anim_set_duration(&animation, duration);
    lv_anim_set_exec_cb(&animation, controls_panel_y_animation);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_start(&animation);
}

void close_controls(bool animated) {
    if (controls_panel == nullptr || controls_backdrop == nullptr) {
        return;
    }
    active_control = nullptr;
    controls_drag_visible = false;
    controls_gesture.set_open(false, lv_tick_get());
    lv_anim_delete(controls_panel, controls_panel_y_animation);
    if (!animated) {
        controls_close_animation_active = false;
        lv_obj_set_y(controls_panel, kControlsPanelHiddenY);
        set_hidden(controls_panel, true);
        set_hidden(controls_backdrop, true);
        update_controls_handle();
        return;
    }

    controls_close_animation_active = true;
    set_hidden(controls_edge_target, true);
    const std::int32_t current_y = lv_obj_get_y(controls_panel);
    const std::uint32_t duration = controls_animation_duration(
        current_y, kControlsPanelHiddenY, kControlsCloseAnimationMs);
    if (duration == 0) {
        finish_controls_close_animation(nullptr);
        return;
    }
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, controls_panel);
    lv_anim_set_values(
        &animation, current_y, kControlsPanelHiddenY);
    lv_anim_set_duration(&animation, duration);
    lv_anim_set_exec_cb(&animation, controls_panel_y_animation);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&animation, finish_controls_close_animation);
    lv_anim_start(&animation);
}

void open_controls() {
    if (controls_panel == nullptr || controls_backdrop == nullptr ||
        !controls_gesture.allowed() || controls_gesture.open()) {
        return;
    }
    controls_close_animation_active = false;
    // Opening is an activity event only. Do not run panel I/O from the LVGL
    // event callback before the control panel becomes visible.
    dispatch_controls_update(false, false, false);
    lv_anim_delete(controls_panel, controls_panel_y_animation);
    set_hidden(controls_edge_target, true);
    show_controls_layer();
    lv_obj_set_y(controls_panel, kControlsPanelHiddenY);
    settle_controls_open();
}

bool touch_point(lv_event_t *event, lv_point_t &point) {
    lv_indev_t *input = lv_event_get_indev(event);
    if (input == nullptr) {
        return false;
    }
    lv_indev_get_point(input, &point);
    return true;
}

void controls_gesture_event(lv_event_t *event) {
    const lv_event_code_t code = lv_event_get_code(event);
    lv_point_t point{};
    if ((code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING &&
         code != LV_EVENT_RELEASED) ||
        !touch_point(event, point)) {
        return;
    }
    const std::uint32_t now_ms = lv_tick_get();
    if (code == LV_EVENT_PRESSED) {
        controls_gesture.press(point.x, point.y, now_ms);
        controls_drag_panel_start_y = controls_panel == nullptr
            ? kControlsPanelHiddenY
            : lv_obj_get_y(controls_panel);
        return;
    }

    if (code == LV_EVENT_PRESSING) {
        const std::int32_t distance =
            controls_gesture.drag_distance_y(point.y);
        const bool moves_towards_open =
            !controls_gesture.open() && distance > 0;
        const bool moves_towards_closed =
            controls_gesture.open() && distance < 0;
        if (!moves_towards_open && !moves_towards_closed) {
            return;
        }
        if (!controls_drag_visible) {
            controls_drag_visible = true;
            controls_close_animation_active = false;
            lv_anim_delete(controls_panel, controls_panel_y_animation);
            show_controls_layer();
            dispatch_controls_update(false, false, false);
        }
        const std::int32_t next_y = std::clamp(
            controls_drag_panel_start_y + distance,
            kControlsPanelHiddenY,
            kControlsPanelShownY);
        lv_obj_set_y(controls_panel, next_y);
        controls_gesture.note_activity(now_ms);
        return;
    }

    const bool was_open = controls_gesture.open();
    const QuickControlsAction action =
        controls_gesture.release(point.x, point.y, now_ms);
    QuickControlsAction settle_action = action;
    if (controls_drag_visible) {
        settle_action = controls_gesture.release_was_accepted()
            ? quick_controls_settle_action(
                  lv_obj_get_y(controls_panel),
                  kControlsPanelHiddenY,
                  kControlsPanelShownY)
            : (was_open
                  ? QuickControlsAction::open
                  : QuickControlsAction::close);
    }
    if (settle_action == QuickControlsAction::open) {
        settle_controls_open();
    } else if (settle_action == QuickControlsAction::close) {
        close_controls(true);
    }
}

void controls_edge_event(lv_event_t *event) {
    controls_gesture_event(event);
    if (lv_event_get_code(event) == LV_EVENT_CLICKED &&
        controls_gesture.allowed() && !controls_gesture.open()) {
        open_controls();
    }
}

void controls_backdrop_event(lv_event_t *event) {
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        close_controls(true);
    }
}

void update_control_from_touch(ControlSlider &control, lv_event_t *event) {
    lv_point_t point{};
    if (!touch_point(event, point) || control.touch_target == nullptr) {
        return;
    }
    lv_area_t area{};
    lv_obj_get_coords(control.touch_target, &area);
    const std::uint8_t value =
        QuickControlsGesture::percent_for_track_position(
            static_cast<std::int32_t>(point.x) - area.x1 -
                kControlSliderTrackTouchInset,
            kControlSliderTrackWidth,
            control.minimum_percent);
    if (control.value_percent == value) {
        return;
    }
    set_control_value(control, value);
    const bool brightness = control.kind == ControlKind::brightness;
    if (brightness) {
        controls_brightness_percent = value;
    } else {
        controls_volume_percent = value;
    }
    dispatch_controls_update(brightness, !brightness, false);
}

void controls_slider_event(lv_event_t *event) {
    const lv_event_code_t code = lv_event_get_code(event);
    auto *control = static_cast<ControlSlider *>(
        lv_event_get_user_data(event));
    if (control == nullptr ||
        (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING &&
         code != LV_EVENT_RELEASED)) {
        return;
    }
    controls_gesture.note_activity(lv_tick_get());
    if (code == LV_EVENT_PRESSED) {
        active_control = control;
        update_control_from_touch(*control, event);
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        if (active_control == control) {
            update_control_from_touch(*control, event);
        }
        return;
    }
    if (code == LV_EVENT_RELEASED) {
        if (active_control == control) {
            update_control_from_touch(*control, event);
        }
        active_control = nullptr;
        dispatch_controls_update(false, false, true);
    }
}

void controls_timer_callback(lv_timer_t *) {
    if (active_control == nullptr &&
        controls_gesture.automatic_close_due(lv_tick_get())) {
        close_controls(true);
    }
}

void set_controls_state_allowed(bool allowed) {
    controls_state_allowed = allowed;
    const bool effective = controls_callback != nullptr &&
        (clock_mode || controls_state_allowed) && !passkey_visible;
    if (!effective && (controls_gesture.open() || controls_drag_visible)) {
        close_controls(false);
    }
    controls_gesture.set_allowed(effective);
    update_controls_handle();
}

void hide_passkey() {
    const bool restore_orientation = passkey_visible;
    if (passkey_overlay != nullptr) {
        lv_obj_add_flag(passkey_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    passkey_visible = false;
    if (restore_orientation) {
        apply_display_orientation();
    }
    set_controls_state_allowed(controls_state_allowed);
    std::fill(passkey_buffer.begin(), passkey_buffer.end(), '\0');
}

void prepare_voice_view() {
    hide_fullscreen_visual();
    hide_passkey();
    recording_spectrum::show(false);
}

lv_obj_t *create_controls_text(
    lv_obj_t *parent, const char *text, lv_color_t color,
    const lv_font_t *font) {
    lv_obj_t *label = lv_label_create(parent);
    set_static_text(label, text);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    return label;
}

void create_control_slider(
    lv_obj_t *parent,
    ControlSlider &control,
    ControlKind kind,
    const char *title_text,
    std::uint8_t minimum_percent,
    std::int32_t title_y,
    std::int32_t slider_y) {
    control = {};
    control.kind = kind;
    control.minimum_percent = minimum_percent;

    lv_obj_t *title = create_controls_text(
        parent, title_text, lv_color_hex(0x8e8e93),
        &lv_font_montserrat_14);
    lv_obj_set_style_text_letter_space(title, 1, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, title_y);

    control.value_label = create_controls_text(
        parent, "", lv_color_hex(0xffffff), &lv_font_montserrat_14);
    lv_obj_set_width(control.value_label, 72);
    lv_obj_set_style_text_align(
        control.value_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align(
        control.value_label, LV_ALIGN_TOP_RIGHT, -24, title_y);

    control.touch_target = lv_obj_create(parent);
    lv_obj_remove_style_all(control.touch_target);
    lv_obj_set_size(
        control.touch_target,
        kControlSliderTouchWidth,
        kControlSliderTouchHeight);
    // Keep the visible track and knob at their original coordinates. Only the
    // transparent input surface grows around them.
    lv_obj_align(
        control.touch_target,
        LV_ALIGN_TOP_MID,
        0,
        slider_y + kControlSliderTouchOffsetY);
    lv_obj_set_style_bg_opa(
        control.touch_target, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(control.touch_target, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(control.touch_target, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_clear_flag(control.touch_target, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *track = lv_obj_create(control.touch_target);
    lv_obj_remove_style_all(track);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(
        track, kControlSliderTrackWidth, kControlSliderTrackHeight);
    lv_obj_align(track, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(
        track, lv_color_hex(0x28282c), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(track, LV_RADIUS_CIRCLE, LV_PART_MAIN);

    control.indicator = lv_obj_create(track);
    lv_obj_remove_style_all(control.indicator);
    lv_obj_clear_flag(control.indicator, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(
        control.indicator, 1, kControlSliderTrackHeight);
    lv_obj_align(control.indicator, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(
        control.indicator, lv_color_hex(0xf2f2f7), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(
        control.indicator, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(
        control.indicator, LV_RADIUS_CIRCLE, LV_PART_MAIN);

    control.knob = lv_obj_create(control.touch_target);
    lv_obj_remove_style_all(control.knob);
    lv_obj_clear_flag(control.knob, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(
        control.knob, kControlSliderKnobSize, kControlSliderKnobSize);
    lv_obj_set_y(
        control.knob,
        (kControlSliderTouchHeight - kControlSliderKnobSize) / 2);
    lv_obj_set_style_bg_color(
        control.knob, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(control.knob, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(control.knob, LV_RADIUS_CIRCLE, LV_PART_MAIN);

    lv_obj_add_event_cb(
        control.touch_target, controls_slider_event, LV_EVENT_ALL,
        &control);
}

void create_quick_controls(lv_obj_t *screen) {
    controls_backdrop = lv_obj_create(screen);
    lv_obj_remove_style_all(controls_backdrop);
    lv_obj_set_size(
        controls_backdrop, image::kDisplayWidth, image::kDisplayHeight);
    lv_obj_set_style_bg_color(
        controls_backdrop, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(controls_backdrop, LV_OPA_60, LV_PART_MAIN);
    lv_obj_add_flag(controls_backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(controls_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(
        controls_backdrop, controls_backdrop_event, LV_EVENT_CLICKED,
        nullptr);
    lv_obj_add_flag(controls_backdrop, LV_OBJ_FLAG_HIDDEN);

    controls_panel = lv_obj_create(screen);
    lv_obj_remove_style_all(controls_panel);
    lv_obj_set_size(
        controls_panel, image::kDisplayWidth, kControlsPanelHeight);
    lv_obj_set_pos(controls_panel, 0, kControlsPanelHiddenY);
    lv_obj_set_style_bg_color(
        controls_panel, lv_color_hex(0x101012), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(controls_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(controls_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(
        controls_panel, lv_color_hex(0x38383d), LV_PART_MAIN);
    lv_obj_set_style_radius(controls_panel, 28, LV_PART_MAIN);
    lv_obj_add_flag(controls_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(controls_panel, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_clear_flag(controls_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(
        controls_panel, controls_gesture_event, LV_EVENT_ALL, nullptr);
    lv_obj_add_flag(controls_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *panel_handle = lv_obj_create(controls_panel);
    lv_obj_remove_style_all(panel_handle);
    lv_obj_clear_flag(panel_handle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(
        panel_handle, kControlsHandleWidth, kControlsHandleHeight);
    lv_obj_set_style_bg_color(
        panel_handle, lv_color_hex(0x6b6b70), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel_handle, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(panel_handle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_align(
        panel_handle, LV_ALIGN_TOP_MID, 0, kControlsPanelHandleY);

    lv_obj_t *title = create_controls_text(
        controls_panel, "CONTROLS", lv_color_hex(0xffffff),
        &lv_font_montserrat_18);
    lv_obj_set_style_text_letter_space(title, 2, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 42);

    create_control_slider(
        controls_panel,
        brightness_control,
        ControlKind::brightness,
        "BRIGHTNESS",
        runtime::DevicePreferences::minimum_brightness_percent,
        82,
        108);
    create_control_slider(
        controls_panel,
        volume_control,
        ControlKind::volume,
        "VOLUME",
        0,
        176,
        202);

    controls_edge_target = lv_obj_create(screen);
    lv_obj_remove_style_all(controls_edge_target);
    lv_obj_set_size(controls_edge_target, image::kDisplayWidth, 34);
    lv_obj_align(controls_edge_target, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(
        controls_edge_target, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(controls_edge_target, LV_OBJ_FLAG_CLICKABLE);
    // The required 48-pixel swipe ends below this 34-pixel target. Keep the
    // original target pressed until release so LVGL delivers the full swipe.
    lv_obj_add_flag(controls_edge_target, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_clear_flag(controls_edge_target, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(
        controls_edge_target, controls_edge_event, LV_EVENT_ALL, nullptr);

    controls_edge_handle = lv_obj_create(controls_edge_target);
    lv_obj_remove_style_all(controls_edge_handle);
    lv_obj_clear_flag(controls_edge_handle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(
        controls_edge_handle, kControlsHandleWidth, kControlsHandleHeight);
    lv_obj_set_style_bg_color(
        controls_edge_handle, lv_color_hex(0x6b6b70), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(
        controls_edge_handle, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(
        controls_edge_handle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_align(
        controls_edge_handle, LV_ALIGN_TOP_MID, 0, kControlsEdgeHandleY);

    sync_controls_values(
        runtime::DevicePreferences::default_brightness_percent,
        runtime::DevicePreferences::default_volume_percent);
    set_hidden(controls_edge_target, true);
    controls_timer = lv_timer_create(controls_timer_callback, 250, nullptr);
}

void create_startup_screen() {
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
}

void create_runtime_screen() {
    lv_obj_t *screen = active_screen();

    content_viewport = lv_obj_create(screen);
    lv_obj_remove_style_all(content_viewport);
    lv_obj_set_size(content_viewport, 336, 270);
    lv_obj_align(content_viewport, LV_ALIGN_TOP_LEFT, 16, 126);
    lv_obj_set_scroll_dir(content_viewport, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content_viewport, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_flag(content_viewport, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(content_viewport, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_flag(content_viewport, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_add_flag(content_viewport, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_clear_flag(content_viewport, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_style_width(content_viewport, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(
        content_viewport, lv_color_hex(0x777777), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(
        content_viewport, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(
        content_viewport, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);

    content_label = lv_label_create(content_viewport);
    lv_obj_set_width(content_label, 336);
    lv_obj_set_height(content_label, LV_SIZE_CONTENT);
    lv_label_set_long_mode(content_label, LV_LABEL_LONG_WRAP);
    lv_obj_clear_flag(content_label, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_color(
        content_label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        content_label, &chatesp_font_18, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(content_label, 7, LV_PART_MAIN);
    lv_obj_set_style_text_align(
        content_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_pos(content_label, 0, 0);
    set_static_text(content_label, content_buffer.data());

    recording_spectrum::create(screen);

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
    lv_obj_set_width(battery_status_label, 48);
    lv_obj_set_style_text_color(
        battery_status_label, lv_color_hex(0x777777), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        battery_status_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(
        battery_status_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align(battery_status_label, LV_ALIGN_BOTTOM_RIGHT, -30, -20);

    battery_icon_label = lv_label_create(screen);
    lv_obj_set_width(battery_icon_label, 24);
    set_static_text(battery_icon_label, LV_SYMBOL_BATTERY_EMPTY);
    lv_obj_set_style_text_color(
        battery_icon_label, lv_color_hex(0x777777), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        battery_icon_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(
        battery_icon_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align_to(
        battery_icon_label,
        battery_status_label,
        LV_ALIGN_OUT_LEFT_MID,
        -4,
        0);

    // LVGL supplies separate standard battery and charge glyphs. Scale and
    // center the charge glyph to make one compact charging-battery symbol.
    battery_charge_label = lv_label_create(screen);
    lv_obj_set_width(battery_charge_label, 24);
    set_static_text(battery_charge_label, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(
        battery_charge_label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        battery_charge_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(
        battery_charge_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_x(
        battery_charge_label, 12, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(
        battery_charge_label, 7, LV_PART_MAIN);
    lv_obj_set_style_transform_scale(
        battery_charge_label, 160, LV_PART_MAIN);
    lv_obj_align_to(
        battery_charge_label, battery_icon_label, LV_ALIGN_CENTER, 0, 0);
    set_hidden(battery_charge_label, true);

#if CHATESP_DEVELOPMENT_MODE
    lv_obj_t *development_status_label = lv_label_create(screen);
    lv_obj_set_width(development_status_label, 64);
    set_static_text(development_status_label, "DEV");
    lv_obj_set_style_text_color(
        development_status_label, lv_color_hex(0x777777), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        development_status_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(
        development_status_label, 1, LV_PART_MAIN);
    lv_obj_set_style_text_align(
        development_status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(development_status_label, LV_ALIGN_BOTTOM_MID, 0, -20);
#endif
    show_footer(RadioIndicator::off, 0, false, 0, false);

    image_overlay = lv_image_create(screen);
    lv_obj_set_size(
        image_overlay, image::kDisplayWidth, image::kDisplayHeight);
    lv_obj_center(image_overlay);
    lv_obj_add_flag(image_overlay, LV_OBJ_FLAG_HIDDEN);

    plot_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(plot_overlay);
    lv_obj_set_size(plot_overlay, 368, 448);
    lv_obj_set_style_bg_color(
        plot_overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(plot_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(plot_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(plot_overlay);

    plot_title_label = lv_label_create(plot_overlay);
    lv_obj_set_width(plot_title_label, 328);
    lv_label_set_long_mode(plot_title_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(
        plot_title_label, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        plot_title_label, &chatesp_font_18, LV_PART_MAIN);
    lv_obj_set_style_text_align(
        plot_title_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(plot_title_label, LV_ALIGN_TOP_MID, 0, 22);

    plot_chart = lv_chart_create(plot_overlay);
    lv_obj_set_size(plot_chart, 302, 300);
    lv_obj_align(plot_chart, LV_ALIGN_TOP_MID, 0, 68);
    lv_chart_set_type(plot_chart, LV_CHART_TYPE_SCATTER);
    lv_chart_set_axis_range(
        plot_chart, LV_CHART_AXIS_PRIMARY_X, 0, 1'000);
    lv_chart_set_axis_range(
        plot_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1'000);
    lv_chart_set_div_line_count(plot_chart, 5, 5);
    lv_obj_set_style_bg_color(
        plot_chart, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(plot_chart, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(
        plot_chart, lv_color_hex(0x777777), LV_PART_MAIN);
    lv_obj_set_style_border_width(plot_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_color(
        plot_chart, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_style_line_width(plot_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_width(plot_chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(plot_chart, 0, 0, LV_PART_INDICATOR);
    plot_series = lv_chart_add_series(
        plot_chart, lv_color_hex(0xffffff), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_series_ext_x_array(
        plot_chart, plot_series, plot_x_values.data());
    lv_chart_set_series_ext_y_array(
        plot_chart, plot_series, plot_y_values.data());

    plot_x_range_label = lv_label_create(plot_overlay);
    lv_obj_set_width(plot_x_range_label, 320);
    lv_obj_set_style_text_color(
        plot_x_range_label, lv_color_hex(0x777777), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        plot_x_range_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(
        plot_x_range_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(plot_x_range_label, LV_ALIGN_BOTTOM_MID, 0, -46);

    plot_y_range_label = lv_label_create(plot_overlay);
    lv_obj_set_width(plot_y_range_label, 320);
    lv_obj_set_style_text_color(
        plot_y_range_label, lv_color_hex(0x777777), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        plot_y_range_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(
        plot_y_range_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(plot_y_range_label, LV_ALIGN_BOTTOM_MID, 0, -22);
    hide_fullscreen_plot();

    create_quick_controls(screen);

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
    create_clock_face(screen);

    sleep_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(sleep_overlay);
    lv_obj_set_size(sleep_overlay, 368, 448);
    lv_obj_set_style_bg_color(
        sleep_overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(
        sleep_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(sleep_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(sleep_overlay);
    lv_obj_add_flag(sleep_overlay, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace

bool start(std::uint8_t brightness_percent) {
    if (!runtime::DevicePreferences{
            brightness_percent,
            runtime::DevicePreferences::default_volume_percent}.valid()) {
        return false;
    }
    lv_display_t *display = bsp_display_start();
    if (display == nullptr || bsp_display_backlight_off() != ESP_OK) {
        return false;
    }
    display_handle = display;
    if (!bsp_display_lock(1000)) {
        return false;
    }
    create_startup_screen();
    show_state(InteractionState::booting);
    lv_refr_now(display);
    bsp_display_unlock();
    const esp_err_t wake_error =
        bsp_display_brightness_set(brightness_percent);
    if (wake_error != ESP_OK) {
        return false;
    }
    // Send one more complete startup frame after panel-on. Do this before the
    // hidden runtime views are built so the user sees the splash at the first
    // reliable CO5300 pixel transfer.
    if (!bsp_display_lock(1000)) {
        return false;
    }
    lv_obj_invalidate(active_screen());
    lv_refr_now(display);
    bsp_display_unlock();
    if (bsp_display_brightness_set(brightness_percent) != ESP_OK) {
        return false;
    }

    // Touch is not needed for the startup view. Probe it only after the first
    // reliable pixels are visible so its I2C transactions do not delay the
    // user's first feedback from a cold start.
    touch_available = bsp_display_start_touch(display) == ESP_OK;

    if (!bsp_display_lock(1000)) {
        (void)bsp_display_backlight_off();
        return false;
    }
    create_runtime_screen();
    show_state(InteractionState::booting);
    bsp_display_unlock();
    return true;
}

bool set_clock_style(const ClockStyle &style) {
    if (!style.valid()) {
        return false;
    }
    clock_style = style;
    apply_clock_style(clock_mode);
    if (clock_root != nullptr) {
        lv_obj_invalidate(clock_root);
    }
    return true;
}

void show_app_mode(AppMode mode, InteractionState chat_state) {
    if (display_handle == nullptr || clock_root == nullptr) {
        return;
    }
    close_controls(false);
    if (mode == AppMode::clock) {
        hide_fullscreen_visual();
        clock_mode = true;
        set_hidden(content_viewport, true);
        if (clock_path_point_count == 0) {
            apply_clock_style(true);
        }
        if (clock_path_timer != nullptr) {
            lv_timer_resume(clock_path_timer);
        }
        apply_display_orientation();
        lv_obj_set_size(clock_root, kClockWidth, kClockHeight);
        if (!clock_face_initialized) {
            show_clock_time(false);
        }
        set_hidden(clock_root, false);
        lv_obj_move_foreground(clock_root);
        set_controls_state_allowed(controls_state_allowed);
        bring_clock_overlays_forward();
        lv_obj_invalidate(clock_root);
        return;
    }

    clock_mode = false;
    set_hidden(content_viewport, false);
    if (clock_path_timer != nullptr) {
        lv_timer_pause(clock_path_timer);
    }
    set_hidden(clock_root, true);
    apply_display_orientation();
    shown_clock_minute = 0xff;
    shown_clock_hour = 0xff;
    shown_clock_second = 0xff;
    shown_clock_millisecond = 0;
    shown_clock_available = false;
    clock_face_initialized = false;
    show_state(chat_state);
}

void show_clock_time(bool available, ClockTime time) {
    if (clock_root == nullptr || (available && !time.valid())) {
        return;
    }
    const bool text_changed =
        !clock_face_initialized || available != shown_clock_available ||
        (available && (time.minute != shown_clock_minute ||
                       time.hour != shown_clock_hour));
    const ClockPathSpan previous_path = shown_clock_path;
    shown_clock_available = available;
    clock_face_initialized = true;
    shown_clock_minute = available ? time.minute : 0xff;
    shown_clock_second = available ? time.second : 0xff;
    shown_clock_hour = available ? time.hour : 0xff;
    shown_clock_millisecond = available ? time.millisecond : 0;
    shown_clock_tick = lv_tick_get();

    if (text_changed && clock_time_label != nullptr) {
        clock_time_buffer = clock_time_text(available, time);
        lv_label_set_text_static(clock_time_label, clock_time_buffer.data());
        lv_obj_center(clock_time_label);
    }

    shown_clock_path = current_clock_path_span();
    if (text_changed || !same_clock_path(previous_path, shown_clock_path)) {
        lv_obj_invalidate(clock_root);
    }
}

bool enable_quick_controls(
    std::uint8_t brightness_percent,
    std::uint8_t volume_percent,
    QuickControlsCallback callback,
    void *context) {
    const runtime::DevicePreferences preferences{
        brightness_percent, volume_percent};
    if (!touch_available || !preferences.valid() || callback == nullptr ||
        controls_panel == nullptr || controls_edge_target == nullptr ||
        controls_timer == nullptr) {
        return false;
    }
    controls_callback = callback;
    controls_callback_context = context;
    sync_controls_values(brightness_percent, volume_percent);
    set_controls_state_allowed(controls_state_allowed);
    return true;
}

void disable_quick_controls() {
    close_controls(false);
    controls_callback = nullptr;
    controls_callback_context = nullptr;
    controls_gesture.set_allowed(false);
    update_controls_handle();
}

void sync_quick_controls(
    std::uint8_t brightness_percent, std::uint8_t volume_percent) {
    const runtime::DevicePreferences preferences{
        brightness_percent, volume_percent};
    if (!preferences.valid()) {
        return;
    }
    sync_controls_values(brightness_percent, volume_percent);
}

void show_state(InteractionState state) {
    if (status_label == nullptr || hint_label == nullptr) {
        return;
    }
    if (state != InteractionState::idle && state != InteractionState::booting) {
        hide_passkey();
    }
    if (state != InteractionState::idle) {
        hide_fullscreen_visual();
    }
    set_controls_state_allowed(controls_allowed_for_state(state));
    if (state == InteractionState::booting ||
        state == InteractionState::recording ||
        state == InteractionState::sleep_pending) {
        set_content({}, kMaximumAnswerBytes, ContentKind::none);
    }
    set_static_text(status_label, status(state));
    set_static_text(hint_label, hint(state));
    show_activity(
        false);
    recording_spectrum::show(state == InteractionState::recording);
}

void show_recording_spectrum(const AudioSpectrum &levels) {
    hide_passkey();
    recording_spectrum::update(levels);
}

void show_transcript(std::string_view transcript) {
    prepare_voice_view();
    show_activity(false);
    set_static_text(status_label, "YOU");
    set_static_text(hint_label, "TRANSCRIPT");
    set_content(
        transcript, kMaximumTranscriptBytes, ContentKind::transcript);
}

void show_answer_stream(std::string_view answer) {
    prepare_voice_view();
    show_activity(false);
    set_static_text(status_label, "ANSWER");
    set_static_text(hint_label, "RECEIVING");
    set_content(answer, kMaximumAnswerBytes, ContentKind::answer);
}

void show_answer(std::string_view answer) {
    prepare_voice_view();
    show_activity(false);
    set_static_text(status_label, "ANSWER");
    set_static_text(hint_label, "PREPARING SPEECH");
    set_content(answer, kMaximumAnswerBytes, ContentKind::answer);
}

void show_answer_notice(std::string_view answer, std::string_view notice) {
    prepare_voice_view();
    show_activity(false);
    set_static_text(status_label, "ANSWER");
    set_hint(notice);
    set_content(answer, kMaximumAnswerBytes, ContentKind::answer);
}

void show_error(std::string_view error) {
    prepare_voice_view();
    show_activity(false);
    set_static_text(status_label, "REQUEST FAILED");
    set_static_text(hint_label, "ERROR DETAILS");
    set_content(error, kMaximumErrorBytes, ContentKind::error);
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
    passkey_visible = true;
    set_controls_state_allowed(controls_state_allowed);
    apply_display_orientation();
    set_static_text(passkey_label, passkey_buffer.data());
    show_activity(false);
    recording_spectrum::show(false);
    lv_obj_move_foreground(passkey_overlay);
    lv_obj_clear_flag(passkey_overlay, LV_OBJ_FLAG_HIDDEN);
}

void show_footer(
    RadioIndicator radio, std::uint8_t signal_band, bool battery_available,
    std::uint8_t battery_percent, bool external_power_connected) {
    const char *radio_text = LV_SYMBOL_WIFI " OFF";
    switch (radio) {
        case RadioIndicator::setup:
            radio_text = LV_SYMBOL_SETTINGS " SETUP";
            break;
        case RadioIndicator::off:
            radio_text = LV_SYMBOL_WIFI " OFF";
            break;
        case RadioIndicator::wifi_connecting:
            radio_text = LV_SYMBOL_WIFI " CONNECTING";
            break;
        case RadioIndicator::wifi_online:
            radio_text = signal_band == 1
                ? LV_SYMBOL_WIFI " |||"
                : (signal_band == 2
                       ? LV_SYMBOL_WIFI " ||"
                       : (signal_band == 3
                              ? LV_SYMBOL_WIFI " |"
                              : LV_SYMBOL_WIFI " ?"));
            break;
        case RadioIndicator::ble_online:
            radio_text = LV_SYMBOL_BLUETOOTH;
            break;
        case RadioIndicator::failed:
            radio_text = LV_SYMBOL_WARNING " RETRY";
            break;
    }
    copy_bounded(
        radio_text,
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
    lv_obj_set_style_text_color(
        battery_icon_label,
        lv_color_hex(external_power_connected ? 0x00ff66 : 0x777777),
        LV_PART_MAIN);
    lv_obj_set_style_text_color(
        battery_status_label,
        lv_color_hex(external_power_connected ? 0x00ff66 : 0x777777),
        LV_PART_MAIN);
    set_static_text(battery_icon_label, battery_icon);
    set_hidden(battery_charge_label, !external_power_connected);
    if (battery_available && battery_percent <= 100) {
        std::snprintf(
            battery_status_buffer.data(),
            battery_status_buffer.size(),
            "%u%%",
            static_cast<unsigned>(battery_percent));
    } else {
        std::snprintf(
            battery_status_buffer.data(),
            battery_status_buffer.size(),
            "--%%");
    }
    set_static_text(battery_status_label, battery_status_buffer.data());
}

bool show_fullscreen_image(image::Rgb565Frame &&frame) {
    if (image_overlay == nullptr || !frame.available()) {
        return false;
    }

    hide_fullscreen_visual();
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
    update_controls_handle();
    if (controls_gesture.open() || controls_drag_visible) {
        lv_obj_move_foreground(controls_backdrop);
        lv_obj_move_foreground(controls_panel);
    } else if (controls_edge_target != nullptr &&
        !lv_obj_has_flag(controls_edge_target, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(controls_edge_target);
    }
    if (passkey_overlay != nullptr &&
        !lv_obj_has_flag(passkey_overlay, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(passkey_overlay);
    }
    lv_obj_invalidate(image_overlay);
    return true;
}

bool show_fullscreen_plot(const agent::PlotData &plot) {
    if (plot_overlay == nullptr || plot_chart == nullptr ||
        plot_series == nullptr || !plot.ready()) {
        return false;
    }
    double minimum_x = plot.x[0];
    double maximum_x = plot.x[0];
    double minimum_y = 0.0;
    double maximum_y = 0.0;
    bool found_y = false;
    bool has_line_segment = false;
    for (std::size_t index = 0; index < plot.count; ++index) {
        if (!std::isfinite(plot.x[index]) || std::isinf(plot.y[index])) {
            return false;
        }
        minimum_x = std::min(minimum_x, plot.x[index]);
        maximum_x = std::max(maximum_x, plot.x[index]);
        if (!std::isnan(plot.y[index])) {
            has_line_segment = has_line_segment ||
                (index != 0 && std::isfinite(plot.y[index - 1]));
            if (!found_y) {
                minimum_y = plot.y[index];
                maximum_y = plot.y[index];
                found_y = true;
            } else {
                minimum_y = std::min(minimum_y, plot.y[index]);
                maximum_y = std::max(maximum_y, plot.y[index]);
            }
        }
    }
    if (!found_y || !has_line_segment) {
        return false;
    }

    hide_fullscreen_visual();
    const long double range_x =
        static_cast<long double>(maximum_x) - minimum_x;
    const long double range_y =
        static_cast<long double>(maximum_y) - minimum_y;
    for (std::size_t index = 0; index < plot.count; ++index) {
        const long double normalized_x = range_x == 0.0L
            ? static_cast<long double>(index) /
                static_cast<long double>(plot.count - 1)
            : (static_cast<long double>(plot.x[index]) - minimum_x) / range_x;
        plot_x_values[index] = static_cast<std::int32_t>(
            std::clamp(normalized_x, 0.0L, 1.0L) * 1'000.0L);
        if (std::isnan(plot.y[index])) {
            plot_y_values[index] = LV_CHART_POINT_NONE;
            continue;
        }
        const long double normalized_y = range_y == 0.0L
            ? 0.5L
            : (static_cast<long double>(plot.y[index]) - minimum_y) / range_y;
        plot_y_values[index] = static_cast<std::int32_t>(
            std::clamp(normalized_y, 0.0L, 1.0L) * 1'000.0L);
    }
    copy_bounded(
        plot.title.empty() ? std::string_view{"PYTHON PLOT"}
                           : std::string_view{plot.title.data(), plot.title.size()},
        plot_title_buffer.data(), plot_title_buffer.size(),
        agent::Limits::max_plot_title_bytes);
    std::snprintf(
        plot_x_range_buffer.data(), plot_x_range_buffer.size(),
        "X  %.5g  TO  %.5g", minimum_x, maximum_x);
    std::snprintf(
        plot_y_range_buffer.data(), plot_y_range_buffer.size(),
        "Y  %.5g  TO  %.5g", minimum_y, maximum_y);
    set_static_text(plot_title_label, plot_title_buffer.data());
    set_static_text(plot_x_range_label, plot_x_range_buffer.data());
    set_static_text(plot_y_range_label, plot_y_range_buffer.data());
    lv_chart_set_point_count(plot_chart, plot.count);
    lv_chart_refresh(plot_chart);
    lv_obj_clear_flag(plot_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(plot_overlay);
    update_controls_handle();
    if (controls_gesture.open() || controls_drag_visible) {
        lv_obj_move_foreground(controls_backdrop);
        lv_obj_move_foreground(controls_panel);
    } else if (controls_edge_target != nullptr &&
        !lv_obj_has_flag(controls_edge_target, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(controls_edge_target);
    }
    if (passkey_overlay != nullptr &&
        !lv_obj_has_flag(passkey_overlay, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_move_foreground(passkey_overlay);
    }
    lv_obj_invalidate(plot_overlay);
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

void hide_fullscreen_visual() {
    hide_fullscreen_image();
    hide_fullscreen_plot();
}

esp_err_t sleep(bool keep_panel_ready) {
    if (display_handle == nullptr || sleep_overlay == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!bsp_display_lock(25)) {
        return ESP_ERR_TIMEOUT;
    }
    close_controls(false);
    lv_obj_remove_flag(sleep_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(sleep_overlay);
    lv_obj_invalidate(sleep_overlay);
    lv_refr_now(display_handle);
    const esp_err_t result = keep_panel_ready
        ? ESP_OK
        : bsp_display_backlight_off();
    bsp_display_unlock();
    return result;
}

esp_err_t wake(
    InteractionState state, std::uint8_t brightness_percent, AppMode mode) {
    if (brightness_percent <
            runtime::DevicePreferences::minimum_brightness_percent ||
        brightness_percent > runtime::DevicePreferences::maximum_percent) {
        return ESP_ERR_INVALID_ARG;
    }
    // A held wake must reach microphone capture without a long display wait.
    if (!bsp_display_lock(25)) {
        return ESP_ERR_TIMEOUT;
    }
    lv_obj_add_flag(sleep_overlay, LV_OBJ_FLAG_HIDDEN);
    hide_fullscreen_visual();
    set_content({}, kMaximumAnswerBytes, ContentKind::none);
    show_app_mode(mode, state);
    // A command acknowledgement does not prove that the CO5300 output stage
    // is active. Replay its bounded initialization table on each deliberate
    // wake. Keep the LVGL lock so its task cannot start a transfer while the
    // command table changes the panel state.
    const esp_err_t wake_error = bsp_display_recover(brightness_percent);
    if (wake_error != ESP_OK) {
        bsp_display_unlock();
        return wake_error;
    }
    // Send one complete frame after panel-on. The sleep overlay left a black
    // frame in panel memory, so the recovery does not expose stale content.
    lv_obj_invalidate(active_screen());
    lv_refr_now(display_handle);
    const esp_err_t brightness_error =
        bsp_display_brightness_set(brightness_percent);
    bsp_display_unlock();
    return brightness_error;
}

esp_err_t set_brightness(std::uint8_t brightness_percent) {
    if (brightness_percent <
            runtime::DevicePreferences::minimum_brightness_percent ||
        brightness_percent > runtime::DevicePreferences::maximum_percent) {
        return ESP_ERR_INVALID_ARG;
    }
    return bsp_display_brightness_set(brightness_percent);
}

}  // namespace chatesp::ui
