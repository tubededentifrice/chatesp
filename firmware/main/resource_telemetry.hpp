#pragma once

#include <cstdint>

#include "task_config.hpp"

namespace chatesp::resource_telemetry {

enum class Point : std::uint8_t {
    display_ready,
    audio_ready,
    ble_start,
    ble_stop,
    wifi_start,
    wifi_stop,
    recording_start,
    recording_stop,
    speech_start,
    speech_stop,
    image_start,
    image_stop,
    sleep_entry,
};

enum class DisplayEvent : std::uint8_t {
    submission,
    completion,
    transfer_error,
    queue_coalesced,
};

void record_point(Point point);
void record_display_event(DisplayEvent event);
void record_task_watermark(task_config::TaskId task);
void log_summary(const char *reason);

}  // namespace chatesp::resource_telemetry
