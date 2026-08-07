#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <unity.h>

#include "chatesp/byte_ring.hpp"
#include "chatesp/pcm16_stream.hpp"
#include "chatesp/pcm_start_policy.hpp"
#include "chatesp/runtime_control.hpp"

namespace {

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
    TEST_ASSERT_TRUE(playback_rate.decided());
    TEST_ASSERT_FALSE(playback_rate.streams_early());
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::runtime::PcmStartDecision::wait),
        static_cast<int>(playback_rate.decision(false)));

    chatesp::runtime::AdaptivePcmStartPolicy below_headroom;
    below_headroom.observe(2'048, 3'000);
    below_headroom.observe(9'600, 3'134);
    TEST_ASSERT_FALSE(below_headroom.streams_early());

    chatesp::runtime::AdaptivePcmStartPolicy above_headroom;
    above_headroom.observe(2'048, 4'000);
    above_headroom.observe(9'600, 4'133);
    TEST_ASSERT_TRUE(above_headroom.streams_early());
}

void test_pcm_start_policy_keeps_a_slow_decision_until_complete() {
    chatesp::runtime::AdaptivePcmStartPolicy policy;
    policy.observe(2'048, 1'000);
    policy.observe(9'600, 2'000);
    policy.observe(100'000, 2'001);

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

void test_interaction_deadline_is_bounded_and_handles_wrap() {
    constexpr std::uint32_t started =
        std::numeric_limits<std::uint32_t>::max() - 49;
    chatesp::runtime::MonotonicDeadline deadline(started, 100);
    TEST_ASSERT_FALSE(deadline.expired(49));
    TEST_ASSERT_TRUE(deadline.expired(50));
}

}  // namespace

void setUp() {}
void tearDown() {}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_stream_joins_a_sample_across_writes);
    RUN_TEST(test_stream_rejects_an_incomplete_final_sample);
    RUN_TEST(test_stream_bounds_each_output_chunk);
    RUN_TEST(test_stream_validates_input_and_keeps_output_failure);
    RUN_TEST(test_byte_ring_wraps_and_wipes_consumed_bytes);
    RUN_TEST(test_byte_ring_bounds_writes_and_discards_private_data);
    RUN_TEST(test_byte_ring_does_not_write_outside_its_storage);
    RUN_TEST(test_pcm_start_policy_waits_for_its_prebuffer);
    RUN_TEST(test_pcm_start_policy_streams_only_with_safe_headroom);
    RUN_TEST(test_pcm_start_policy_keeps_a_slow_decision_until_complete);
    RUN_TEST(test_pcm_start_policy_handles_millisecond_wrap);
    RUN_TEST(test_poweroff_gate_routes_a_cold_wake_press);
    RUN_TEST(test_poweroff_gate_cancels_a_sleep_boundary);
    RUN_TEST(test_poweroff_gate_keeps_development_sleep_out_of_poweroff);
    RUN_TEST(test_interaction_deadline_is_bounded_and_handles_wrap);
    return UNITY_END();
}
