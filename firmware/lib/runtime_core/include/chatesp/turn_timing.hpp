#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace chatesp {
namespace runtime {

enum class TurnPhase : std::uint8_t {
    button_release,
    network_ready,
    stt_start,
    stt_finish,
    route_start,
    first_answer_text,
    playback_start,
    playback_finish,
    image_ready,
    completion,
    count,
};

// This record can contain only monotonic times. It cannot hold user content,
// service URLs, credentials, or device identifiers.
class TurnTiming {
public:
    void reset(std::uint32_t released_at_ms);
    void mark(TurnPhase phase, std::uint32_t at_ms);
    void set_rssi_band(std::uint8_t band) {
        rssi_band_.store(band > 3 ? 0 : band, std::memory_order_release);
    }
    void note_tool_round() {
        tool_rounds_.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] bool marked(TurnPhase phase) const;
    [[nodiscard]] std::uint32_t elapsed(TurnPhase phase) const;
    [[nodiscard]] bool format_summary(char *output, std::size_t capacity) const;

private:
    static constexpr std::size_t kPhaseCount =
        static_cast<std::size_t>(TurnPhase::count);
    std::array<std::atomic<std::uint32_t>, kPhaseCount> times_{};
    std::array<std::atomic<bool>, kPhaseCount> present_{};
    std::atomic<std::uint8_t> rssi_band_{0};
    std::atomic<std::uint8_t> tool_rounds_{0};
};

}  // namespace runtime
}  // namespace chatesp
