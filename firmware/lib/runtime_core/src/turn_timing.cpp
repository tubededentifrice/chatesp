#include "chatesp/turn_timing.hpp"

#include <cstdio>

namespace chatesp {
namespace runtime {

void TurnTiming::reset(std::uint32_t released_at_ms) {
    for (std::size_t index = 0; index < kPhaseCount; ++index) {
        times_[index].store(0, std::memory_order_relaxed);
        present_[index].store(false, std::memory_order_relaxed);
    }
    rssi_band_.store(0, std::memory_order_relaxed);
    tool_rounds_.store(0, std::memory_order_relaxed);
    mark(TurnPhase::button_release, released_at_ms);
}

void TurnTiming::mark(TurnPhase phase, std::uint32_t at_ms) {
    const std::size_t index = static_cast<std::size_t>(phase);
    if (index >= kPhaseCount) {
        return;
    }
    if (!present_[index].load(std::memory_order_acquire)) {
        times_[index].store(at_ms, std::memory_order_relaxed);
        present_[index].store(true, std::memory_order_release);
    }
}

bool TurnTiming::marked(TurnPhase phase) const {
    const std::size_t index = static_cast<std::size_t>(phase);
    return index < kPhaseCount &&
        present_[index].load(std::memory_order_acquire);
}

std::uint32_t TurnTiming::elapsed(TurnPhase phase) const {
    const std::size_t release =
        static_cast<std::size_t>(TurnPhase::button_release);
    const std::size_t index = static_cast<std::size_t>(phase);
    if (index >= kPhaseCount ||
        !present_[release].load(std::memory_order_acquire) ||
        !present_[index].load(std::memory_order_acquire)) {
        return 0;
    }
    return times_[index].load(std::memory_order_acquire) -
        times_[release].load(std::memory_order_acquire);
}

bool TurnTiming::format_summary(char *output, std::size_t capacity) const {
    if (output == nullptr || capacity == 0 ||
        !marked(TurnPhase::button_release)) {
        return false;
    }
    const int size = std::snprintf(
        output, capacity,
        "LATENCY network_ms=%u stt_start_ms=%u stt_finish_ms=%u "
        "first_text_ms=%u first_audio_ms=%u audio_finish_ms=%u "
        "image_ms=%u turn_ms=%u rssi_band=%u tool_rounds=%u",
        static_cast<unsigned>(elapsed(TurnPhase::network_ready)),
        static_cast<unsigned>(elapsed(TurnPhase::stt_start)),
        static_cast<unsigned>(elapsed(TurnPhase::stt_finish)),
        static_cast<unsigned>(elapsed(TurnPhase::first_answer_text)),
        static_cast<unsigned>(elapsed(TurnPhase::playback_start)),
        static_cast<unsigned>(elapsed(TurnPhase::playback_finish)),
        static_cast<unsigned>(elapsed(TurnPhase::image_ready)),
        static_cast<unsigned>(elapsed(TurnPhase::completion)),
        static_cast<unsigned>(rssi_band_.load(std::memory_order_acquire)),
        static_cast<unsigned>(tool_rounds_.load(std::memory_order_acquire)));
    return size > 0 && static_cast<std::size_t>(size) < capacity;
}

}  // namespace runtime
}  // namespace chatesp
