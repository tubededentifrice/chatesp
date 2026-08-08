#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "chatesp/audio_spectrum.hpp"
#include "lvgl.h"

namespace chatesp::ui::recording_spectrum {
namespace {

constexpr std::int32_t kWidth = 336;
constexpr std::int32_t kHeight = 198;
constexpr std::int32_t kPlotHeight = 190;
constexpr std::int32_t kBarWidth = 12;
constexpr std::int32_t kBarGap = 6;
constexpr std::int32_t kBarsWidth =
    static_cast<std::int32_t>(kAudioSpectrumBandCount) * kBarWidth +
    static_cast<std::int32_t>(kAudioSpectrumBandCount - 1) * kBarGap;

lv_obj_t *root = nullptr;
AudioSpectrum shown_levels{};
AudioSpectrum held_peaks{};

void draw_block(
    lv_layer_t *layer, const lv_area_t &area,
    lv_draw_rect_dsc_t &descriptor, std::int32_t x, std::int32_t y,
    std::int32_t width, std::int32_t height) {
    const lv_area_t block{
        area.x1 + x,
        area.y1 + y,
        area.x1 + x + width - 1,
        area.y1 + y + height - 1,
    };
    lv_draw_rect(layer, &descriptor, &block);
}

void draw(lv_event_t *event) {
    if (lv_event_get_code(event) != LV_EVENT_DRAW_MAIN) {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_obj_t *object = lv_event_get_current_target_obj(event);
    if (layer == nullptr || object == nullptr) {
        return;
    }
    lv_area_t area;
    lv_obj_get_coords(object, &area);

    lv_draw_rect_dsc_t descriptor;
    lv_draw_rect_dsc_init(&descriptor);
    descriptor.base.layer = layer;
    descriptor.bg_opa = LV_OPA_COVER;
    descriptor.radius = LV_RADIUS_CIRCLE;

    for (std::size_t band = 0; band < shown_levels.size(); ++band) {
        const std::int32_t x =
            (kWidth - kBarsWidth) / 2 +
            static_cast<std::int32_t>(band) * (kBarWidth + kBarGap);
        descriptor.bg_color = lv_color_hex(0x101012);
        draw_block(
            layer, area, descriptor, x, 0, kBarWidth, kPlotHeight);

        const std::uint8_t level = shown_levels[band];
        const std::int32_t height = level == 0
            ? 2
            : std::max<std::int32_t>(
                  4, static_cast<std::int32_t>(level) *
                      kPlotHeight / 100);
        descriptor.bg_color = lv_color_hex(0xf2f2f7);
        draw_block(
            layer, area, descriptor, x, kPlotHeight - height,
            kBarWidth, height);

        const std::int32_t peak_y = held_peaks[band] == 0
            ? kPlotHeight - 2
            : std::max<std::int32_t>(
                  0,
                  kPlotHeight -
                      static_cast<std::int32_t>(held_peaks[band]) *
                          kPlotHeight / 100 -
                      4);
        descriptor.bg_color = lv_color_hex(0x6b6b70);
        draw_block(
            layer, area, descriptor, x, peak_y, kBarWidth, 2);
    }

    descriptor.radius = 0;
    descriptor.bg_color = lv_color_hex(0x28282c);
    draw_block(
        layer, area, descriptor, (kWidth - kBarsWidth) / 2,
        kPlotHeight + 7, kBarsWidth, 1);
}

}  // namespace

inline void reset() {
    shown_levels.fill(0);
    held_peaks.fill(0);
    if (root != nullptr) {
        lv_obj_invalidate(root);
    }
}

inline void create(lv_obj_t *screen) {
    root = lv_obj_create(screen);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, kWidth, kHeight);
    lv_obj_align(root, LV_ALIGN_TOP_LEFT, 16, 132);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, draw, LV_EVENT_DRAW_MAIN, nullptr);
    reset();
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
}

inline void show(bool visible) {
    if (root == nullptr) {
        return;
    }
    if (!visible) {
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    reset();
    lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
}

inline void update(const AudioSpectrum &levels) {
    if (root == nullptr) {
        return;
    }
    for (std::size_t band = 0; band < shown_levels.size(); ++band) {
        const std::uint16_t target =
            std::min<std::uint16_t>(levels[band], 100);
        const std::uint16_t current = shown_levels[band];
        shown_levels[band] = static_cast<std::uint8_t>(
            target >= current
            ? (3 * target + current + 2) / 4
            : (target + 3 * current + 2) / 4);

        if (shown_levels[band] >= held_peaks[band]) {
            held_peaks[band] = shown_levels[band];
        } else {
            held_peaks[band] = static_cast<std::uint8_t>(
                held_peaks[band] > 4 ? held_peaks[band] - 4 : 0);
        }
    }
    lv_obj_invalidate(root);
}

}  // namespace chatesp::ui::recording_spectrum
