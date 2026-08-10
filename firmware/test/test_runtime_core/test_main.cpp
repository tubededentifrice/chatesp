#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include <unity.h>

#include "chatesp/byte_ring.hpp"
#include "chatesp/ble_controller.hpp"
#include "chatesp/clock_network_transition.hpp"
#include "chatesp/crash_trace.hpp"
#include "chatesp/device_preferences.hpp"
#include "chatesp/pcm16_stream.hpp"
#include "chatesp/pcm_start_policy.hpp"
#include "chatesp/runtime_control.hpp"
#include "chatesp/speech_segmenter.hpp"
#include "chatesp/speech_segment_queue.hpp"
#include "chatesp/turn_timing.hpp"

namespace {

class SegmentCollector final : public chatesp::runtime::SpeechSegmentSink {
public:
    bool push_speech_segment(const char *text, std::size_t size) override {
        if (!accept || count >= values.size() || text == nullptr ||
            size > chatesp::runtime::SpeechSegmenter::kMaximumSegmentBytes) {
            return false;
        }
        std::memcpy(values[count].data(), text, size);
        values[count][size] = '\0';
        sizes[count] = size;
        ++count;
        return true;
    }

    std::array<
        std::array<
            char,
            chatesp::runtime::SpeechSegmenter::kMaximumSegmentBytes + 1>,
        chatesp::runtime::SpeechSegmenter::kMaximumSegments> values{};
    std::array<
        std::size_t,
        chatesp::runtime::SpeechSegmenter::kMaximumSegments> sizes{};
    std::size_t count = 0;
    bool accept = true;
};

struct CollectedSamples {
    std::array<std::int16_t, 1'100> values{};
    std::size_t size = 0;
    std::size_t calls = 0;
    std::size_t largest_call = 0;
    bool accept = true;
};

bool collect(
    void *context, const std::int16_t *samples,
    std::size_t sample_count) {
    auto *output = static_cast<CollectedSamples *>(context);
    ++output->calls;
    if (!output->accept ||
        sample_count > output->values.size() - output->size) {
        return false;
    }
    if (sample_count > output->largest_call) {
        output->largest_call = sample_count;
    }
    for (std::size_t index = 0; index < sample_count; ++index) {
        output->values[output->size++] = samples[index];
    }
    return true;
}

void test_device_preferences_have_a_strict_versioned_record() {
    chatesp::runtime::DevicePreferences preferences;
    TEST_ASSERT_TRUE(preferences.valid());
    const auto encoded =
        chatesp::runtime::encode_device_preferences(preferences);
    const std::uint8_t expected[] = {
        'C', 'E', 'D', 'P', 1, 65, 70, 0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(
        expected, encoded.data(), encoded.size());

    chatesp::runtime::DevicePreferences decoded{5, 0};
    TEST_ASSERT_TRUE(chatesp::runtime::decode_device_preferences(
        encoded.data(), encoded.size(), decoded));
    TEST_ASSERT_EQUAL_UINT8(65, decoded.brightness_percent);
    TEST_ASSERT_EQUAL_UINT8(70, decoded.volume_percent);
}

void test_crash_trace_keeps_bounded_reset_history() {
    chatesp::runtime::CrashTraceStore trace{};
    TEST_ASSERT_FALSE(chatesp::runtime::crash_trace_valid(trace));

    chatesp::runtime::crash_trace_begin_boot(trace, 1);
    TEST_ASSERT_TRUE(chatesp::runtime::crash_trace_valid(trace));
    auto active_index = chatesp::runtime::crash_trace_active_index(trace);
    TEST_ASSERT_LESS_THAN(trace.boots.size(), active_index);
    TEST_ASSERT_EQUAL_UINT32(1, trace.boots[active_index].sequence);
    for (std::uint32_t index = 0;
         index < chatesp::runtime::kCrashTraceEventCount + 4;
         ++index) {
        TEST_ASSERT_TRUE(chatesp::runtime::crash_trace_mark(
            trace, chatesp::runtime::CrashEvent::ble_stop_requested, index));
    }
    const auto first_index = active_index;
    TEST_ASSERT_EQUAL_UINT8(
        chatesp::runtime::kCrashTraceEventCount,
        trace.boots[first_index].event_count);
    TEST_ASSERT_EQUAL_UINT32(
        4, trace.boots[first_index]
               .events[trace.boots[first_index].next_event]
               .at_ms);

    chatesp::runtime::crash_trace_begin_boot(trace, 9);
    TEST_ASSERT_TRUE(chatesp::runtime::crash_trace_valid(trace));
    active_index = chatesp::runtime::crash_trace_active_index(trace);
    TEST_ASSERT_EQUAL_UINT8(0, trace.boots[first_index].active);
    TEST_ASSERT_EQUAL_UINT32(9, trace.boots[first_index].outcome_reset_reason);
    TEST_ASSERT_EQUAL_UINT32(2, trace.boots[active_index].sequence);

    chatesp::runtime::crash_trace_begin_boot(trace, 3);
    chatesp::runtime::crash_trace_begin_boot(trace, 4);
    TEST_ASSERT_TRUE(chatesp::runtime::crash_trace_valid(trace));
    active_index = chatesp::runtime::crash_trace_active_index(trace);
    TEST_ASSERT_EQUAL_UINT32(4, trace.boots[active_index].sequence);
}

void test_crash_trace_rejects_corruption() {
    chatesp::runtime::CrashTraceStore trace{};
    chatesp::runtime::crash_trace_begin_boot(trace, 1);
    TEST_ASSERT_TRUE(chatesp::runtime::crash_trace_valid(trace));
    const auto active_index =
        chatesp::runtime::crash_trace_active_index(trace);
    ++trace.boots[active_index].sequence;
    TEST_ASSERT_FALSE(chatesp::runtime::crash_trace_valid(trace));
    TEST_ASSERT_FALSE(chatesp::runtime::crash_trace_mark(
        trace, chatesp::runtime::CrashEvent::runtime_ready, 10));
}

void test_crash_trace_recovers_without_erasing_older_records() {
    chatesp::runtime::CrashTraceStore trace{};
    chatesp::runtime::crash_trace_begin_boot(trace, 1);
    chatesp::runtime::crash_trace_begin_boot(trace, 2);
    const auto damaged_index =
        chatesp::runtime::crash_trace_active_index(trace);
    ++trace.boots[damaged_index].checksum;
    TEST_ASSERT_FALSE(chatesp::runtime::crash_trace_valid(trace));

    chatesp::runtime::crash_trace_begin_boot(trace, 9);
    TEST_ASSERT_TRUE(chatesp::runtime::crash_trace_valid(trace));
    const auto active_index =
        chatesp::runtime::crash_trace_active_index(trace);
    TEST_ASSERT_EQUAL_UINT32(2, trace.boots[active_index].sequence);
    std::size_t completed_count = 0;
    for (const auto &boot : trace.boots) {
        if (chatesp::runtime::crash_boot_record_valid(boot) &&
            boot.active == 0) {
            ++completed_count;
        }
    }
    TEST_ASSERT_EQUAL_size_t(1, completed_count);
}

void test_crash_trace_heartbeat_does_not_change_event_checksum() {
    chatesp::runtime::CrashTraceStore trace{};
    chatesp::runtime::crash_trace_begin_boot(trace, 1);
    const auto active_index =
        chatesp::runtime::crash_trace_active_index(trace);
    const std::uint32_t checksum = trace.boots[active_index].checksum;
    TEST_ASSERT_TRUE(chatesp::runtime::crash_trace_heartbeat(trace, 5'000));
    TEST_ASSERT_EQUAL_UINT32(
        5'000, trace.boots[active_index].last_heartbeat_ms);
    TEST_ASSERT_EQUAL_UINT32(checksum, trace.boots[active_index].checksum);
    TEST_ASSERT_TRUE(chatesp::runtime::crash_trace_valid(trace));
}

void test_device_preferences_reject_invalid_or_unknown_records() {
    chatesp::runtime::DevicePreferences preferences;
    auto encoded = chatesp::runtime::encode_device_preferences(preferences);
    chatesp::runtime::DevicePreferences output;

    TEST_ASSERT_FALSE(chatesp::runtime::decode_device_preferences(
        nullptr, encoded.size(), output));
    TEST_ASSERT_FALSE(chatesp::runtime::decode_device_preferences(
        encoded.data(), encoded.size() - 1, output));
    encoded[4] = 2;
    TEST_ASSERT_FALSE(chatesp::runtime::decode_device_preferences(
        encoded.data(), encoded.size(), output));
    encoded[4] = 1;
    encoded[5] = 4;
    TEST_ASSERT_FALSE(chatesp::runtime::decode_device_preferences(
        encoded.data(), encoded.size(), output));
    encoded[5] = 5;
    encoded[6] = 101;
    TEST_ASSERT_FALSE(chatesp::runtime::decode_device_preferences(
        encoded.data(), encoded.size(), output));
    encoded[6] = 100;
    encoded[7] = 1;
    TEST_ASSERT_FALSE(chatesp::runtime::decode_device_preferences(
        encoded.data(), encoded.size(), output));
}

void test_clock_with_local_time_joins_recording_network_before_ble_restart() {
    const auto transition = chatesp::runtime::clock_network_transition(
        true, true, true);
    TEST_ASSERT_EQUAL_size_t(2, transition.size);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::ClockNetworkTransitionStep::
            join_recording_worker),
        static_cast<int>(transition.steps[0]));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::ClockNetworkTransitionStep::
            stop_network_and_restart_ble),
        static_cast<int>(transition.steps[1]));
}

void test_clock_without_local_time_joins_recording_network_before_access() {
    const auto transition = chatesp::runtime::clock_network_transition(
        true, false, true);
    TEST_ASSERT_EQUAL_size_t(2, transition.size);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::ClockNetworkTransitionStep::
            join_recording_worker),
        static_cast<int>(transition.steps[0]));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::ClockNetworkTransitionStep::
            acquire_local_time),
        static_cast<int>(transition.steps[1]));
}

void test_stream_joins_a_sample_across_writes() {
    chatesp::runtime::Pcm16Stream stream;
    CollectedSamples output;
    const std::uint8_t first[] = {0x34};
    const std::uint8_t second[] = {0x12, 0xFE, 0xFF};

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::Pcm16Stream::Status::none),
        static_cast<int>(stream.write(first, sizeof(first), collect, &output)));
    TEST_ASSERT_EQUAL_size_t(0, output.size);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::Pcm16Stream::Status::none),
        static_cast<int>(
            stream.write(second, sizeof(second), collect, &output)));
    TEST_ASSERT_EQUAL_size_t(2, output.size);
    TEST_ASSERT_EQUAL_INT16(0x1234, output.values[0]);
    TEST_ASSERT_EQUAL_INT16(-2, output.values[1]);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::Pcm16Stream::Status::none),
        static_cast<int>(stream.finish()));
}

void test_stream_rejects_an_incomplete_final_sample() {
    chatesp::runtime::Pcm16Stream stream;
    CollectedSamples output;
    const std::uint8_t data[] = {0x01, 0x00, 0x02};

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::Pcm16Stream::Status::none),
        static_cast<int>(stream.write(data, sizeof(data), collect, &output)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(
            chatesp::runtime::Pcm16Stream::Status::incomplete_sample),
        static_cast<int>(stream.finish()));

    stream.reset();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::Pcm16Stream::Status::none),
        static_cast<int>(stream.finish()));
}

void test_stream_bounds_each_output_chunk() {
    chatesp::runtime::Pcm16Stream stream;
    CollectedSamples output;
    constexpr std::size_t sample_count =
        chatesp::runtime::Pcm16Stream::kMaxChunkSamples * 2 + 3;
    std::array<std::uint8_t, sample_count * 2> data{};
    for (std::size_t index = 0; index < sample_count; ++index) {
        data[index * 2] = static_cast<std::uint8_t>(index & 0xFFU);
        data[index * 2 + 1] =
            static_cast<std::uint8_t>((index >> 8U) & 0xFFU);
    }

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::Pcm16Stream::Status::none),
        static_cast<int>(
            stream.write(data.data(), data.size(), collect, &output)));
    TEST_ASSERT_EQUAL_size_t(sample_count, output.size);
    TEST_ASSERT_EQUAL_size_t(3, output.calls);
    TEST_ASSERT_EQUAL_size_t(
        chatesp::runtime::Pcm16Stream::kMaxChunkSamples,
        output.largest_call);
    for (std::size_t index = 0; index < sample_count; ++index) {
        TEST_ASSERT_EQUAL_INT16(
            static_cast<std::int16_t>(index), output.values[index]);
    }
}

void test_stream_validates_input_and_keeps_output_failure() {
    chatesp::runtime::Pcm16Stream stream;
    CollectedSamples output;
    const std::uint8_t data[] = {0x01, 0x00};

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(
            chatesp::runtime::Pcm16Stream::Status::invalid_argument),
        static_cast<int>(stream.write(nullptr, 1, collect, &output)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(
            chatesp::runtime::Pcm16Stream::Status::invalid_argument),
        static_cast<int>(stream.write(data, sizeof(data), nullptr, &output)));

    output.accept = false;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(
            chatesp::runtime::Pcm16Stream::Status::output_failed),
        static_cast<int>(stream.write(data, sizeof(data), collect, &output)));
    output.accept = true;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(
            chatesp::runtime::Pcm16Stream::Status::output_failed),
        static_cast<int>(stream.write(data, sizeof(data), collect, &output)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(
            chatesp::runtime::Pcm16Stream::Status::output_failed),
        static_cast<int>(stream.finish()));
}

void test_byte_ring_wraps_and_wipes_consumed_bytes() {
    std::array<std::uint8_t, 8> storage{};
    chatesp::runtime::ByteRing ring;
    TEST_ASSERT_TRUE(ring.reset(storage.data(), storage.size()));

    const std::uint8_t first[] = {1, 2, 3, 4, 5, 6};
    TEST_ASSERT_EQUAL_size_t(
        sizeof(first), ring.write(first, sizeof(first)));
    std::array<std::uint8_t, 4> output{};
    TEST_ASSERT_EQUAL_size_t(
        output.size(), ring.read(output.data(), output.size()));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(first, output.data(), output.size());
    TEST_ASSERT_EQUAL_UINT8(0, storage[0]);
    TEST_ASSERT_EQUAL_UINT8(0, storage[3]);

    const std::uint8_t second[] = {7, 8, 9, 10, 11, 12};
    TEST_ASSERT_EQUAL_size_t(
        sizeof(second), ring.write(second, sizeof(second)));
    TEST_ASSERT_EQUAL_size_t(storage.size(), ring.size());
    std::array<std::uint8_t, 8> wrapped{};
    TEST_ASSERT_EQUAL_size_t(
        wrapped.size(), ring.read(wrapped.data(), wrapped.size()));
    const std::uint8_t expected[] = {5, 6, 7, 8, 9, 10, 11, 12};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, wrapped.data(), wrapped.size());
    for (std::uint8_t value : storage) {
        TEST_ASSERT_EQUAL_UINT8(0, value);
    }
}

void test_byte_ring_bounds_writes_and_discards_private_data() {
    std::array<std::uint8_t, 4> storage{};
    chatesp::runtime::ByteRing ring;
    TEST_ASSERT_FALSE(ring.reset(nullptr, storage.size()));
    TEST_ASSERT_TRUE(ring.reset(storage.data(), storage.size()));
    const std::uint8_t input[] = {1, 2, 3, 4, 5};
    TEST_ASSERT_EQUAL_size_t(
        storage.size(), ring.write(input, sizeof(input)));
    TEST_ASSERT_EQUAL_size_t(0, ring.free_size());
    ring.discard();
    TEST_ASSERT_EQUAL_size_t(0, ring.size());
    TEST_ASSERT_EQUAL_size_t(storage.size(), ring.free_size());
    for (std::uint8_t value : storage) {
        TEST_ASSERT_EQUAL_UINT8(0, value);
    }
}

void test_byte_ring_does_not_write_outside_its_storage() {
    std::array<std::uint8_t, 10> guarded{};
    guarded.front() = 0xA5;
    guarded.back() = 0x5A;
    chatesp::runtime::ByteRing ring;
    TEST_ASSERT_TRUE(ring.reset(guarded.data() + 1, guarded.size() - 2));

    const std::uint8_t first[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    TEST_ASSERT_EQUAL_size_t(8, ring.write(first, sizeof(first)));
    std::array<std::uint8_t, 5> output{};
    TEST_ASSERT_EQUAL_size_t(
        output.size(), ring.read(output.data(), output.size()));
    const std::uint8_t second[] = {10, 11, 12, 13, 14, 15};
    TEST_ASSERT_EQUAL_size_t(5, ring.write(second, sizeof(second)));
    ring.discard();

    TEST_ASSERT_EQUAL_HEX8(0xA5, guarded.front());
    TEST_ASSERT_EQUAL_HEX8(0x5A, guarded.back());
}

void test_byte_ring_can_remove_an_unplayed_tail() {
    std::array<std::uint8_t, 8> storage{};
    chatesp::runtime::ByteRing ring;
    TEST_ASSERT_TRUE(ring.reset(storage.data(), storage.size()));
    const std::uint8_t input[] = {1, 2, 3, 4, 5, 6};
    TEST_ASSERT_EQUAL_size_t(sizeof(input), ring.write(input, sizeof(input)));
    TEST_ASSERT_TRUE(ring.unwrite(3));
    TEST_ASSERT_EQUAL_size_t(3, ring.size());
    TEST_ASSERT_FALSE(ring.unwrite(4));
    std::array<std::uint8_t, 3> output{};
    TEST_ASSERT_EQUAL_size_t(3, ring.read(output.data(), output.size()));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(input, output.data(), output.size());
}

void test_pcm_start_policy_waits_for_its_prebuffer() {
    chatesp::runtime::AdaptivePcmStartPolicy policy;
    policy.observe(2'048, 1'000);
    policy.observe(9'599, 1'100);

    TEST_ASSERT_FALSE(policy.decided());
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::PcmStartDecision::wait),
        static_cast<int>(policy.decision(false)));
}

void test_pcm_start_policy_streams_only_with_safe_headroom() {
    chatesp::runtime::AdaptivePcmStartPolicy fast;
    fast.observe(2'048, 1'000);
    fast.observe(9'600, 1'100);
    TEST_ASSERT_TRUE(fast.decided());
    TEST_ASSERT_TRUE(fast.streams_early());
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::PcmStartDecision::stream_now),
        static_cast<int>(fast.decision(false)));

    chatesp::runtime::AdaptivePcmStartPolicy playback_rate;
    playback_rate.observe(2'048, 2'000);
    playback_rate.observe(9'600, 2'200);
    TEST_ASSERT_FALSE(playback_rate.decided());
    playback_rate.observe(24'000, 2'500);
    TEST_ASSERT_TRUE(playback_rate.decided());
    TEST_ASSERT_FALSE(playback_rate.streams_early());
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::PcmStartDecision::wait),
        static_cast<int>(playback_rate.decision(false)));

    chatesp::runtime::AdaptivePcmStartPolicy below_headroom;
    below_headroom.observe(2'048, 3'000);
    below_headroom.observe(9'600, 3'134);
    below_headroom.observe(24'000, 3'417);
    TEST_ASSERT_FALSE(below_headroom.streams_early());

    chatesp::runtime::AdaptivePcmStartPolicy above_headroom;
    above_headroom.observe(2'048, 4'000);
    above_headroom.observe(9'600, 4'150);
    TEST_ASSERT_FALSE(above_headroom.decided());
    above_headroom.observe(24'000, 4'400);
    TEST_ASSERT_TRUE(above_headroom.streams_early());
}

void test_pcm_start_policy_keeps_a_steady_slow_decision_until_complete() {
    chatesp::runtime::AdaptivePcmStartPolicy policy;
    policy.observe(2'048, 1'000);
    policy.observe(9'600, 2'000);
    TEST_ASSERT_FALSE(policy.decided());
    policy.observe(24'000, 3'000);
    policy.observe(100'000, 3'001);

    TEST_ASSERT_FALSE(policy.streams_early());
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::PcmStartDecision::wait),
        static_cast<int>(policy.decision(false)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(
            chatesp::runtime::PcmStartDecision::start_complete),
        static_cast<int>(policy.decision(true)));
}

void test_pcm_start_policy_handles_millisecond_wrap() {
    chatesp::runtime::AdaptivePcmStartPolicy policy;
    policy.observe(
        2'048, std::numeric_limits<std::uint32_t>::max() - 49);
    policy.observe(9'600, 50);

    TEST_ASSERT_TRUE(policy.streams_early());
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::PcmStartDecision::stream_now),
        static_cast<int>(policy.decision(false)));
}

void test_poweroff_gate_routes_a_cold_wake_press() {
    chatesp::runtime::PoweroffGate gate;
    gate.begin_sleep();
    TEST_ASSERT_TRUE(gate.mark_poweroff_ready());
    TEST_ASSERT_TRUE(gate.poweroff_ready());
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::ButtonRoute::wake),
        static_cast<int>(gate.button_down()));
    TEST_ASSERT_FALSE(gate.poweroff_ready());
}

void test_poweroff_gate_cancels_a_sleep_boundary() {
    chatesp::runtime::PoweroffGate gate;
    gate.begin_sleep();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::ButtonRoute::wake),
        static_cast<int>(gate.button_down()));
    TEST_ASSERT_FALSE(gate.mark_poweroff_ready());
    TEST_ASSERT_FALSE(gate.poweroff_ready());
}

void test_poweroff_gate_keeps_development_sleep_out_of_poweroff() {
    chatesp::runtime::PoweroffGate gate;
    gate.begin_sleep();
    TEST_ASSERT_TRUE(gate.mark_soft_sleep());
    TEST_ASSERT_FALSE(gate.poweroff_ready());
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::ButtonRoute::wake),
        static_cast<int>(gate.button_down()));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::ButtonRoute::normal),
        static_cast<int>(gate.button_down()));
}

void test_async_shutdown_gate_bounds_one_worker_lifecycle() {
    chatesp::runtime::AsyncShutdownGate gate;
    TEST_ASSERT_TRUE(gate.begin());
    TEST_ASSERT_TRUE(gate.running());
    TEST_ASSERT_FALSE(gate.completed());
    TEST_ASSERT_FALSE(gate.begin());
    TEST_ASSERT_FALSE(gate.consume_completion());

    gate.complete();
    TEST_ASSERT_FALSE(gate.running());
    TEST_ASSERT_TRUE(gate.completed());
    TEST_ASSERT_TRUE(gate.consume_completion());
    TEST_ASSERT_FALSE(gate.completed());
    TEST_ASSERT_TRUE(gate.begin());
    TEST_ASSERT_TRUE(gate.cancel_begin());
    TEST_ASSERT_FALSE(gate.running());
}

void test_ble_controller_coalesces_one_requested_state() {
    chatesp::runtime::BleControllerPlanner planner;
    const auto first = planner.request(
        chatesp::runtime::BleControllerTarget::running);
    const auto repeated = planner.request(
        chatesp::runtime::BleControllerTarget::running);

    TEST_ASSERT_EQUAL_UINT32(first, repeated);
    const auto work = planner.begin_next();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::BleControllerOperation::start),
        static_cast<int>(work.operation));
    TEST_ASSERT_EQUAL_UINT32(first, work.generation);
    TEST_ASSERT_FALSE(planner.begin_next().valid());
}

void test_ble_controller_stops_a_superseded_successful_start() {
    chatesp::runtime::BleControllerPlanner planner;
    (void)planner.request(chatesp::runtime::BleControllerTarget::running);
    const auto start = planner.begin_next();
    const auto stop_generation = planner.request(
        chatesp::runtime::BleControllerTarget::stopped);

    TEST_ASSERT_TRUE(planner.complete(start, true));
    TEST_ASSERT_TRUE(planner.actual_running());
    const auto stop = planner.begin_next();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::BleControllerOperation::stop),
        static_cast<int>(stop.operation));
    TEST_ASSERT_EQUAL_UINT32(stop_generation, stop.generation);
    TEST_ASSERT_TRUE(planner.complete(stop, true));
    TEST_ASSERT_FALSE(planner.actual_running());
}

void test_ble_controller_restarts_after_superseded_successful_stop() {
    chatesp::runtime::BleControllerPlanner planner(true);
    (void)planner.request(chatesp::runtime::BleControllerTarget::stopped);
    const auto stop = planner.begin_next();
    const auto start_generation = planner.request(
        chatesp::runtime::BleControllerTarget::running);

    TEST_ASSERT_TRUE(planner.complete(stop, true));
    TEST_ASSERT_FALSE(planner.actual_running());
    const auto start = planner.begin_next();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::BleControllerOperation::start),
        static_cast<int>(start.operation));
    TEST_ASSERT_EQUAL_UINT32(start_generation, start.generation);
}

void test_ble_controller_ignores_a_superseded_stop_failure() {
    chatesp::runtime::BleControllerPlanner planner(true);
    (void)planner.request(chatesp::runtime::BleControllerTarget::stopped);
    const auto stop = planner.begin_next();
    (void)planner.request(chatesp::runtime::BleControllerTarget::running);

    TEST_ASSERT_TRUE(planner.complete(stop, false));
    TEST_ASSERT_TRUE(planner.actual_running());
    TEST_ASSERT_FALSE(planner.current_request_failed());
    TEST_ASSERT_FALSE(planner.begin_next().valid());
}

void test_ble_controller_retries_only_after_a_new_request() {
    chatesp::runtime::BleControllerPlanner planner;
    const auto first_generation = planner.request(
        chatesp::runtime::BleControllerTarget::running);
    const auto first = planner.begin_next();
    TEST_ASSERT_TRUE(planner.complete(first, false));
    TEST_ASSERT_TRUE(planner.current_request_failed());
    TEST_ASSERT_FALSE(planner.begin_next().valid());

    const auto retry_generation = planner.request(
        chatesp::runtime::BleControllerTarget::running);
    TEST_ASSERT_EQUAL_UINT32(first_generation + 1, retry_generation);
    TEST_ASSERT_FALSE(planner.current_request_failed());
    TEST_ASSERT_TRUE(planner.begin_next().valid());
}

void test_ble_controller_ignores_a_superseded_start_failure() {
    chatesp::runtime::BleControllerPlanner planner;
    (void)planner.request(chatesp::runtime::BleControllerTarget::running);
    const auto start = planner.begin_next();
    (void)planner.request(chatesp::runtime::BleControllerTarget::stopped);

    TEST_ASSERT_TRUE(planner.complete(start, false));
    TEST_ASSERT_FALSE(planner.actual_running());
    TEST_ASSERT_FALSE(planner.current_request_failed());
    TEST_ASSERT_FALSE(planner.begin_next().valid());
}

void test_ble_controller_generation_wrap_keeps_the_latest_target() {
    chatesp::runtime::BleControllerPlanner planner(
        false, std::numeric_limits<std::uint32_t>::max());
    const auto wrapped = planner.request(
        chatesp::runtime::BleControllerTarget::running);
    TEST_ASSERT_EQUAL_UINT32(0, wrapped);
    const auto start = planner.begin_next();
    TEST_ASSERT_EQUAL_UINT32(0, start.generation);

    const auto stop_generation = planner.request(
        chatesp::runtime::BleControllerTarget::stopped);
    TEST_ASSERT_EQUAL_UINT32(1, stop_generation);
    TEST_ASSERT_TRUE(planner.complete(start, true));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::BleControllerOperation::stop),
        static_cast<int>(planner.begin_next().operation));
}

void test_interaction_deadline_is_bounded_and_handles_wrap() {
    constexpr std::uint32_t started =
        std::numeric_limits<std::uint32_t>::max() - 49;
    chatesp::runtime::MonotonicDeadline deadline(started, 100);
    TEST_ASSERT_FALSE(deadline.expired(49));
    TEST_ASSERT_TRUE(deadline.expired(50));
}

void test_phone_proxy_keeps_a_saved_bond_through_a_long_recording() {
    TEST_ASSERT_TRUE(chatesp::runtime::keep_ble_during_recording(
        true, true, 1'000, 31'000, 2'000));
    TEST_ASSERT_FALSE(chatesp::runtime::keep_ble_during_recording(
        true, false, 1'000, 3'000, 2'000));
    TEST_ASSERT_FALSE(chatesp::runtime::keep_ble_during_recording(
        false, true, 1'000, 1'100, 2'000));
}

void test_phone_proxy_initial_recording_grace_handles_wrap() {
    constexpr std::uint32_t started =
        std::numeric_limits<std::uint32_t>::max() - 999;
    TEST_ASSERT_TRUE(chatesp::runtime::keep_ble_during_recording(
        true, false, started, 999, 2'000));
    TEST_ASSERT_FALSE(chatesp::runtime::keep_ble_during_recording(
        true, false, started, 1'000, 2'000));
}

void test_speech_segmenter_accepts_cumulative_split_updates() {
    chatesp::runtime::SpeechSegmenter segmenter;
    SegmentCollector output;
    TEST_ASSERT_TRUE(segmenter.update("Hello wor", 9, output));
    TEST_ASSERT_EQUAL_size_t(0, output.count);
    const char complete[] = "Hello world. Next?";
    TEST_ASSERT_TRUE(
        segmenter.update(complete, sizeof(complete) - 1, output));
    TEST_ASSERT_EQUAL_size_t(1, output.count);
    TEST_ASSERT_EQUAL_STRING("Hello world.", output.values[0].data());
    TEST_ASSERT_TRUE(segmenter.finish(output));
    TEST_ASSERT_EQUAL_size_t(2, output.count);
    TEST_ASSERT_EQUAL_STRING("Next?", output.values[1].data());
}

void test_speech_segmenter_sends_one_complete_remainder() {
    chatesp::runtime::SpeechSegmenter segmenter;
    SegmentCollector output;
    const char text[] =
        "First sentence. Second sentence! Third sentence? Final clause.";
    TEST_ASSERT_TRUE(segmenter.update(text, sizeof(text) - 1, output));
    TEST_ASSERT_EQUAL_size_t(1, output.count);
    TEST_ASSERT_EQUAL_STRING("First sentence.", output.values[0].data());
    TEST_ASSERT_TRUE(segmenter.finish(output));
    TEST_ASSERT_EQUAL_size_t(2, output.count);
    TEST_ASSERT_EQUAL_STRING(
        "Second sentence! Third sentence? Final clause.",
        output.values[1].data());
}

void test_speech_segmenter_bounds_a_long_first_sentence() {
    chatesp::runtime::SpeechSegmenter segmenter;
    SegmentCollector output;
    std::array<char, 181> text{};
    text.fill('a');
    text[150] = ' ';
    TEST_ASSERT_TRUE(segmenter.update(text.data(), 180, output));
    TEST_ASSERT_EQUAL_size_t(1, output.count);
    TEST_ASSERT_EQUAL_size_t(150, output.sizes[0]);
    TEST_ASSERT_TRUE(segmenter.finish(output));
    TEST_ASSERT_EQUAL_size_t(2, output.count);
    TEST_ASSERT_EQUAL_size_t(29, output.sizes[1]);
}

void test_speech_segmenter_bounds_segments_and_spoken_bytes() {
    chatesp::runtime::SpeechSegmenter segmenter;
    SegmentCollector output;
    std::array<char, 641> text{};
    for (std::size_t segment = 0; segment < 4; ++segment) {
        for (std::size_t index = 0; index < 159; ++index) {
            text[segment * 160 + index] = 'a';
        }
        text[segment * 160 + 159] = '!';
    }
    TEST_ASSERT_TRUE(segmenter.update(text.data(), 640, output));
    TEST_ASSERT_TRUE(segmenter.finish(output));
    TEST_ASSERT_EQUAL_size_t(2, output.count);
    TEST_ASSERT_EQUAL_size_t(640, segmenter.emitted_bytes());
    TEST_ASSERT_TRUE(segmenter.update(text.data(), 641, output));
    TEST_ASSERT_EQUAL_size_t(2, output.count);
    TEST_ASSERT_EQUAL_size_t(160, output.sizes[0]);
    TEST_ASSERT_EQUAL_size_t(480, output.sizes[1]);
}

void test_speech_segmenter_propagates_sink_failure() {
    chatesp::runtime::SpeechSegmenter segmenter;
    SegmentCollector output;
    output.accept = false;
    TEST_ASSERT_FALSE(segmenter.update("No!", 3, output));
    segmenter.reset();
    output.accept = true;
    TEST_ASSERT_TRUE(segmenter.update("Yes!", 4, output));
    TEST_ASSERT_EQUAL_STRING("Yes!", output.values[0].data());
}

void test_speech_segmenter_keeps_utf8_code_points_complete() {
    chatesp::runtime::SpeechSegmenter segmenter;
    SegmentCollector output;
    std::array<char, 166> text{};
    for (std::size_t index = 0; index < 158; ++index) {
        text[index] = 'a';
    }
    text[158] = static_cast<char>(0xE2);
    text[159] = static_cast<char>(0x82);
    text[160] = static_cast<char>(0xAC);
    text[161] = ' ';
    text[162] = 'o';
    text[163] = 'k';
    text[164] = '!';
    TEST_ASSERT_TRUE(segmenter.update(text.data(), 165, output));
    TEST_ASSERT_TRUE(segmenter.finish(output));
    TEST_ASSERT_EQUAL_size_t(2, output.count);
    TEST_ASSERT_EQUAL_size_t(158, output.sizes[0]);
    TEST_ASSERT_EQUAL_HEX8(0xE2, output.values[1][0]);
    TEST_ASSERT_EQUAL_HEX8(0x82, output.values[1][1]);
    TEST_ASSERT_EQUAL_HEX8(0xAC, output.values[1][2]);
}

void test_speech_queue_orders_segments_and_applies_backpressure() {
    chatesp::runtime::SpeechSegmentQueue queue;
    TEST_ASSERT_TRUE(queue.push_speech_segment("one", 3));
    TEST_ASSERT_TRUE(queue.push_speech_segment("two", 3));
    TEST_ASSERT_TRUE(queue.full());
    TEST_ASSERT_FALSE(queue.push_speech_segment("three", 5));

    std::array<
        char,
        chatesp::runtime::SpeechSegmentQueue::kSegmentBytes + 1> output{};
    std::size_t size = 0;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::SpeechQueueResult::ready),
        static_cast<int>(queue.pop(output.data(), output.size() - 1, size)));
    TEST_ASSERT_EQUAL_STRING("one", output.data());
    TEST_ASSERT_TRUE(queue.push_speech_segment("three", 5));
}

void test_speech_queue_finish_cancel_and_reset_are_bounded() {
    chatesp::runtime::SpeechSegmentQueue queue;
    std::array<
        char,
        chatesp::runtime::SpeechSegmentQueue::kSegmentBytes + 1> output{};
    std::size_t size = 0;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::SpeechQueueResult::empty),
        static_cast<int>(queue.pop(output.data(), output.size() - 1, size)));
    TEST_ASSERT_TRUE(queue.push_speech_segment("private", 7));
    queue.discard_pending_and_finish();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::SpeechQueueResult::finished),
        static_cast<int>(queue.pop(output.data(), output.size() - 1, size)));
    queue.reset();
    TEST_ASSERT_TRUE(queue.push_speech_segment("again", 5));
    queue.cancel();
    TEST_ASSERT_TRUE(queue.cancelled());
    TEST_ASSERT_TRUE(queue.empty());
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::SpeechQueueResult::cancelled),
        static_cast<int>(queue.pop(output.data(), output.size() - 1, size)));
}

void test_turn_timing_contains_only_phase_durations() {
    chatesp::runtime::TurnTiming timing;
    timing.reset(std::numeric_limits<std::uint32_t>::max() - 49);
    timing.mark(chatesp::runtime::TurnPhase::network_ready, 25);
    timing.mark(chatesp::runtime::TurnPhase::first_answer_text, 75);
    timing.mark(chatesp::runtime::TurnPhase::playback_start, 125);
    timing.mark(chatesp::runtime::TurnPhase::completion, 225);
    timing.set_rssi_band(1);
    std::array<char, 256> output{};
    TEST_ASSERT_TRUE(timing.format_summary(output.data(), output.size()));
    TEST_ASSERT_NOT_NULL(std::strstr(output.data(), "network_ms=75"));
    TEST_ASSERT_NOT_NULL(std::strstr(output.data(), "first_audio_ms=175"));
    TEST_ASSERT_NOT_NULL(std::strstr(output.data(), "turn_ms=275"));
    TEST_ASSERT_NOT_NULL(std::strstr(output.data(), "rssi_band=1"));
    TEST_ASSERT_NULL(std::strstr(output.data(), "text="));
    TEST_ASSERT_NULL(std::strstr(output.data(), "url="));
    TEST_ASSERT_NULL(std::strstr(output.data(), "key="));
}

}  // namespace

void setUp() {}
void tearDown() {}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_crash_trace_keeps_bounded_reset_history);
    RUN_TEST(test_crash_trace_rejects_corruption);
    RUN_TEST(test_crash_trace_recovers_without_erasing_older_records);
    RUN_TEST(test_crash_trace_heartbeat_does_not_change_event_checksum);
    RUN_TEST(test_device_preferences_have_a_strict_versioned_record);
    RUN_TEST(test_device_preferences_reject_invalid_or_unknown_records);
    RUN_TEST(
        test_clock_with_local_time_joins_recording_network_before_ble_restart);
    RUN_TEST(
        test_clock_without_local_time_joins_recording_network_before_access);
    RUN_TEST(test_stream_joins_a_sample_across_writes);
    RUN_TEST(test_stream_rejects_an_incomplete_final_sample);
    RUN_TEST(test_stream_bounds_each_output_chunk);
    RUN_TEST(test_stream_validates_input_and_keeps_output_failure);
    RUN_TEST(test_byte_ring_wraps_and_wipes_consumed_bytes);
    RUN_TEST(test_byte_ring_bounds_writes_and_discards_private_data);
    RUN_TEST(test_byte_ring_does_not_write_outside_its_storage);
    RUN_TEST(test_byte_ring_can_remove_an_unplayed_tail);
    RUN_TEST(test_pcm_start_policy_waits_for_its_prebuffer);
    RUN_TEST(test_pcm_start_policy_streams_only_with_safe_headroom);
    RUN_TEST(
        test_pcm_start_policy_keeps_a_steady_slow_decision_until_complete);
    RUN_TEST(test_pcm_start_policy_handles_millisecond_wrap);
    RUN_TEST(test_poweroff_gate_routes_a_cold_wake_press);
    RUN_TEST(test_poweroff_gate_cancels_a_sleep_boundary);
    RUN_TEST(test_poweroff_gate_keeps_development_sleep_out_of_poweroff);
    RUN_TEST(test_async_shutdown_gate_bounds_one_worker_lifecycle);
    RUN_TEST(test_ble_controller_coalesces_one_requested_state);
    RUN_TEST(test_ble_controller_stops_a_superseded_successful_start);
    RUN_TEST(test_ble_controller_restarts_after_superseded_successful_stop);
    RUN_TEST(test_ble_controller_ignores_a_superseded_stop_failure);
    RUN_TEST(test_ble_controller_retries_only_after_a_new_request);
    RUN_TEST(test_ble_controller_ignores_a_superseded_start_failure);
    RUN_TEST(test_ble_controller_generation_wrap_keeps_the_latest_target);
    RUN_TEST(test_interaction_deadline_is_bounded_and_handles_wrap);
    RUN_TEST(test_phone_proxy_keeps_a_saved_bond_through_a_long_recording);
    RUN_TEST(test_phone_proxy_initial_recording_grace_handles_wrap);
    RUN_TEST(test_speech_segmenter_accepts_cumulative_split_updates);
    RUN_TEST(test_speech_segmenter_sends_one_complete_remainder);
    RUN_TEST(test_speech_segmenter_bounds_a_long_first_sentence);
    RUN_TEST(test_speech_segmenter_bounds_segments_and_spoken_bytes);
    RUN_TEST(test_speech_segmenter_propagates_sink_failure);
    RUN_TEST(test_speech_segmenter_keeps_utf8_code_points_complete);
    RUN_TEST(test_speech_queue_orders_segments_and_applies_backpressure);
    RUN_TEST(test_speech_queue_finish_cancel_and_reset_are_bounded);
    RUN_TEST(test_turn_timing_contains_only_phase_durations);
    return UNITY_END();
}
