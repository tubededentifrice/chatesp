#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <unity.h>

#include "chatesp/pcm16_stream.hpp"
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
    RUN_TEST(test_poweroff_gate_routes_a_cold_wake_press);
    RUN_TEST(test_poweroff_gate_cancels_a_sleep_boundary);
    RUN_TEST(test_poweroff_gate_keeps_development_sleep_out_of_poweroff);
    RUN_TEST(test_interaction_deadline_is_bounded_and_handles_wrap);
    return UNITY_END();
}
