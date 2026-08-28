#include "resource_telemetry.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power_control.hpp"

namespace chatesp::resource_telemetry {
namespace {

constexpr char kTag[] = "resources";
constexpr std::size_t kTaskCount =
    static_cast<std::size_t>(task_config::TaskId::count);

std::array<std::atomic<std::uint32_t>, kTaskCount> s_task_minimum_free{};
std::atomic<std::uint32_t> s_internal_minimum{
    std::numeric_limits<std::uint32_t>::max()};
std::atomic<std::uint32_t> s_internal_largest_minimum{
    std::numeric_limits<std::uint32_t>::max()};
std::atomic<std::uint32_t> s_psram_minimum{
    std::numeric_limits<std::uint32_t>::max()};
std::atomic<std::uint32_t> s_internal_current{0};
std::atomic<std::uint32_t> s_internal_largest_current{0};
std::atomic<std::uint32_t> s_psram_current{0};
std::atomic<std::uint8_t> s_latest_point{
    static_cast<std::uint8_t>(Point::display_ready)};
std::array<std::atomic<std::uint32_t>, 4> s_display_counters{};

void observe_minimum(
    std::atomic<std::uint32_t> &minimum, std::uint32_t value) {
    std::uint32_t current = minimum.load(std::memory_order_relaxed);
    while (value < current &&
           !minimum.compare_exchange_weak(
               current, value, std::memory_order_relaxed)) {
    }
}

void increment_saturating(std::atomic<std::uint32_t> &counter) {
    std::uint32_t current = counter.load(std::memory_order_relaxed);
    while (current != std::numeric_limits<std::uint32_t>::max() &&
           !counter.compare_exchange_weak(
               current, current + 1, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

const char *point_name(Point point) {
    switch (point) {
        case Point::display_ready: return "display_ready";
        case Point::audio_ready: return "audio_ready";
        case Point::ble_start: return "ble_start";
        case Point::ble_stop: return "ble_stop";
        case Point::wifi_start: return "wifi_start";
        case Point::wifi_stop: return "wifi_stop";
        case Point::recording_start: return "recording_start";
        case Point::recording_stop: return "recording_stop";
        case Point::speech_start: return "speech_start";
        case Point::speech_stop: return "speech_stop";
        case Point::image_start: return "image_start";
        case Point::image_stop: return "image_stop";
        case Point::sleep_entry: return "sleep_entry";
    }
    return "unknown";
}

void sample_resources() {
    const std::uint32_t internal = static_cast<std::uint32_t>(
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    const std::uint32_t largest = static_cast<std::uint32_t>(
        heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    const std::uint32_t psram = static_cast<std::uint32_t>(
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_internal_current.store(internal, std::memory_order_relaxed);
    s_internal_largest_current.store(largest, std::memory_order_relaxed);
    s_psram_current.store(psram, std::memory_order_relaxed);
    observe_minimum(s_internal_minimum, internal);
    observe_minimum(s_internal_largest_minimum, largest);
    observe_minimum(s_psram_minimum, psram);
}

}  // namespace

void record_point(Point point) {
    sample_resources();
    s_latest_point.store(
        static_cast<std::uint8_t>(point), std::memory_order_relaxed);
}

void record_display_event(DisplayEvent event) {
    const std::size_t index = static_cast<std::size_t>(event);
    if (index < s_display_counters.size()) {
        increment_saturating(s_display_counters[index]);
    }
}

void record_task_watermark(task_config::TaskId task) {
    const std::size_t index = static_cast<std::size_t>(task);
    if (index >= s_task_minimum_free.size()) {
        return;
    }
    const std::uint32_t free_bytes = static_cast<std::uint32_t>(
        uxTaskGetStackHighWaterMark(nullptr));
    const std::uint32_t encoded =
        free_bytes == std::numeric_limits<std::uint32_t>::max()
        ? free_bytes
        : free_bytes + 1U;
    std::uint32_t current =
        s_task_minimum_free[index].load(std::memory_order_relaxed);
    if (current == 0) {
        (void)s_task_minimum_free[index].compare_exchange_strong(
            current, encoded, std::memory_order_relaxed);
    }
    observe_minimum(s_task_minimum_free[index], encoded);
}

void log_summary(const char *reason) {
    // A lifecycle point gives correlation. A summary also needs the current
    // values at the time of the report, not stale values from that point.
    sample_resources();
    const Point latest_point = static_cast<Point>(
        s_latest_point.load(std::memory_order_relaxed));
    ESP_LOGI(
        kTag,
        "Summary %s: point=%s internal=%u internal_min=%u largest=%u "
        "largest_min=%u psram=%u psram_min=%u",
        reason == nullptr ? "periodic" : reason,
        point_name(latest_point),
        static_cast<unsigned>(
            s_internal_current.load(std::memory_order_relaxed)),
        static_cast<unsigned>(
            s_internal_minimum.load(std::memory_order_relaxed)),
        static_cast<unsigned>(
            s_internal_largest_current.load(std::memory_order_relaxed)),
        static_cast<unsigned>(
            s_internal_largest_minimum.load(std::memory_order_relaxed)),
        static_cast<unsigned>(
            s_psram_current.load(std::memory_order_relaxed)),
        static_cast<unsigned>(s_psram_minimum.load(std::memory_order_relaxed)));
    ESP_LOGI(
        kTag,
        "Display: submission=%u completion=%u transfer_error=%u "
        "queue_coalesced=%u pmic_i2c_error=%u battery_sample_error=%u",
        static_cast<unsigned>(s_display_counters[
            static_cast<std::size_t>(DisplayEvent::submission)]
                                  .load(std::memory_order_relaxed)),
        static_cast<unsigned>(s_display_counters[
            static_cast<std::size_t>(DisplayEvent::completion)]
                                  .load(std::memory_order_relaxed)),
        static_cast<unsigned>(s_display_counters[
            static_cast<std::size_t>(DisplayEvent::transfer_error)]
                                  .load(std::memory_order_relaxed)),
        static_cast<unsigned>(s_display_counters[
            static_cast<std::size_t>(DisplayEvent::queue_coalesced)]
                                  .load(std::memory_order_relaxed)),
        static_cast<unsigned>(power::i2c_error_count()),
        static_cast<unsigned>(power::battery_sample_failure_count()));
    bsp_board_diagnostics_t board{};
    if (bsp_board_diagnostics_get(&board) == ESP_OK) {
        ESP_LOGI(
            kTag,
            "Board: touch_ok=%u touch_error=%u command_ok=%u "
            "command_error=%u recovery_ok=%u recovery_error=%u "
            "lock_timeout=%u",
            static_cast<unsigned>(board.touch_read_ok),
            static_cast<unsigned>(board.touch_read_error),
            static_cast<unsigned>(board.display_command_ok),
            static_cast<unsigned>(board.display_command_error),
            static_cast<unsigned>(board.display_recovery_ok),
            static_cast<unsigned>(board.display_recovery_error),
            static_cast<unsigned>(board.display_lock_timeout));
    }
    for (std::size_t index = 0; index < s_task_minimum_free.size(); ++index) {
        const std::uint32_t encoded =
            s_task_minimum_free[index].load(std::memory_order_relaxed);
        if (encoded == 0) {
            continue;
        }
        const std::uint32_t free_bytes = encoded ==
                std::numeric_limits<std::uint32_t>::max()
            ? encoded
            : encoded - 1U;
        ESP_LOGI(
            kTag, "Task %s: stack_min_free=%u configured=%u",
            task_config::all_tasks[index]->name,
            static_cast<unsigned>(free_bytes),
            static_cast<unsigned>(
                task_config::all_tasks[index]->stack_bytes));
    }
}

}  // namespace chatesp::resource_telemetry
