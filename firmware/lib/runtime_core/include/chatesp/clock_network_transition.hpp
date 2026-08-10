#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace chatesp {
namespace runtime {

enum class ClockNetworkTransitionStep : std::uint8_t {
    join_recording_worker,
    acquire_local_time,
    stop_network_and_restart_ble,
};

struct ClockNetworkTransition {
    std::array<ClockNetworkTransitionStep, 2> steps{};
    std::size_t size = 0;
};

[[nodiscard]] constexpr ClockNetworkTransition clock_network_transition(
    bool recording_worker_active, bool local_time_ready,
    bool wifi_configured) {
    ClockNetworkTransition transition;
    if (recording_worker_active) {
        transition.steps[transition.size++] =
            ClockNetworkTransitionStep::join_recording_worker;
    }
    transition.steps[transition.size++] =
        !local_time_ready && wifi_configured
            ? ClockNetworkTransitionStep::acquire_local_time
            : ClockNetworkTransitionStep::stop_network_and_restart_ble;
    return transition;
}

}  // namespace runtime
}  // namespace chatesp
