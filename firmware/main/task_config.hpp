#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace chatesp::task_config {

enum class TaskId : std::size_t {
    voice_runtime,
    lvgl,
    ble_passkey_ui,
    display_control,
    ble_control,
    startup_services,
    image_download,
    wifi_recording,
    network_context,
    tts_requests,
    deferred_ui,
    tts_playback,
    ble_stop,
    count,
};

struct TaskSpec {
    TaskId id;
    const char *name;
    std::uint32_t stack_bytes;
    UBaseType_t priority;
    BaseType_t core;
    std::uint32_t stack_caps;
};

// These values describe the reviewed provisional task layout. Change a value
// only with a hardware measurement receipt and the matching stack-watermark
// result.
inline constexpr TaskSpec voice_runtime{
    TaskId::voice_runtime, "voice_runtime", 28 * 1024, 5, 1,
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT};
inline constexpr TaskSpec lvgl{
    TaskId::lvgl, "lvgl", 7 * 1024, 4, tskNO_AFFINITY,
    MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT};
inline constexpr TaskSpec ble_passkey_ui{
    TaskId::ble_passkey_ui, "ble_passkey_ui", 8 * 1024, 6, 1,
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT};
inline constexpr TaskSpec display_control{
    TaskId::display_control, "display_control", 8 * 1024, 4, 0,
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT};
inline constexpr TaskSpec ble_control{
    TaskId::ble_control, "ble_control", 16 * 1024, 4, 0,
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT};
inline constexpr TaskSpec startup_services{
    TaskId::startup_services, "startup_services", 12 * 1024, 2, 0,
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT};
inline constexpr TaskSpec image_download{
    TaskId::image_download, "image_download", 20 * 1024, 3, 0,
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT};
inline constexpr TaskSpec wifi_recording{
    TaskId::wifi_recording, "wifi_recording", 6 * 1024, 3, 0,
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT};
inline constexpr TaskSpec network_context{
    TaskId::network_context, "network_context", 20 * 1024, 3, 0,
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT};
inline constexpr TaskSpec tts_requests{
    TaskId::tts_requests, "tts_requests", 16 * 1024, 5, 1,
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT};
inline constexpr TaskSpec deferred_ui{
    TaskId::deferred_ui, "deferred_ui", 10 * 1024, 1, 0,
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT};
inline constexpr TaskSpec tts_playback{
    TaskId::tts_playback, "tts_playback", 16 * 1024, 6, 1,
    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT};
inline constexpr TaskSpec ble_stop{
    TaskId::ble_stop, "ble_stop", 4 * 1024, 5, tskNO_AFFINITY,
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT};

inline constexpr std::array<const TaskSpec *,
                            static_cast<std::size_t>(TaskId::count)>
    all_tasks{
        &voice_runtime,
        &lvgl,
        &ble_passkey_ui,
        &display_control,
        &ble_control,
        &startup_services,
        &image_download,
        &wifi_recording,
        &network_context,
        &tts_requests,
        &deferred_ui,
        &tts_playback,
        &ble_stop,
    };

inline lvgl_port_cfg_t lvgl_port_config() {
    return {
        .task_priority = static_cast<int>(lvgl.priority),
        .task_stack = static_cast<int>(lvgl.stack_bytes),
        // esp_lvgl_port uses -1 for no affinity. FreeRTOS uses
        // tskNO_AFFINITY, which is not negative on this ESP-IDF version.
        .task_affinity = lvgl.core == tskNO_AFFINITY
                             ? -1
                             : static_cast<int>(lvgl.core),
        .task_max_sleep_ms = 500,
        .task_stack_caps = lvgl.stack_caps,
        .timer_period_ms = 5,
    };
}

inline BaseType_t create(
    TaskFunction_t entry,
    const TaskSpec &spec,
    void *context,
    TaskHandle_t *handle) {
    if ((spec.stack_caps & MALLOC_CAP_SPIRAM) != 0) {
        return xTaskCreatePinnedToCoreWithCaps(
            entry, spec.name, spec.stack_bytes, context, spec.priority,
            handle, spec.core, spec.stack_caps);
    }
    return xTaskCreatePinnedToCore(
        entry, spec.name, spec.stack_bytes, context, spec.priority,
        handle, spec.core);
}

}  // namespace chatesp::task_config
