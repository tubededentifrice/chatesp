#include <cstdint>
#include <limits>

#include <unity.h>

#include "chatesp/interaction_state.hpp"

using chatesp::InteractionConfig;
using chatesp::InteractionState;
using chatesp::InteractionStateMachine;

void setUp() {}
void tearDown() {}

void test_short_press_requests_sleep() {
    InteractionStateMachine machine;
    machine.ready(1000);
    machine.button_down(1100);
    machine.button_up(1300);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(InteractionState::sleep_pending),
        static_cast<int>(machine.state()));
}

void test_hold_records_until_release() {
    InteractionStateMachine machine;
    machine.ready(0);
    machine.button_down(10);
    machine.tick(359);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(InteractionState::idle),
        static_cast<int>(machine.state()));
    machine.tick(360);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(InteractionState::recording),
        static_cast<int>(machine.state()));
    machine.button_up(1000);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(InteractionState::transcribing),
        static_cast<int>(machine.state()));
}

void test_release_at_hold_threshold_without_tick_requests_sleep() {
    InteractionStateMachine machine;
    machine.ready(0);
    machine.button_down(10);
    machine.button_up(360);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(InteractionState::sleep_pending),
        static_cast<int>(machine.state()));
}

void test_inactivity_sleeps_only_from_idle() {
    InteractionStateMachine machine;
    machine.ready(100);
    machine.tick(30'099);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(InteractionState::idle),
        static_cast<int>(machine.state()));
    machine.tick(30'100);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(InteractionState::sleep_pending),
        static_cast<int>(machine.state()));

    machine.ready(50'000);
    machine.button_down(50'001);
    machine.tick(50'351);
    machine.tick(90'000);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(InteractionState::recording),
        static_cast<int>(machine.state()));
}

void test_inactivity_handles_millisecond_wrap() {
    InteractionStateMachine machine;
    const std::uint32_t start = std::numeric_limits<std::uint32_t>::max() - 99;
    machine.ready(start);
    machine.tick(29'899);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(InteractionState::idle),
        static_cast<int>(machine.state()));
    machine.tick(29'900);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(InteractionState::sleep_pending),
        static_cast<int>(machine.state()));
}

void test_busy_flow_returns_to_fresh_idle_timer() {
    InteractionStateMachine machine;
    machine.ready(0);
    machine.button_down(1);
    machine.tick(351);
    machine.button_up(400);
    machine.transcription_ready(800);
    machine.tool_started(1200);
    machine.speech_started(1500);
    machine.interaction_finished(2000);
    machine.tick(31'999);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(InteractionState::idle),
        static_cast<int>(machine.state()));
    machine.tick(32'000);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(InteractionState::sleep_pending),
        static_cast<int>(machine.state()));
}

void test_error_returns_to_idle_after_visible_period() {
    InteractionStateMachine machine{InteractionConfig{350, 30'000, 2'200}};
    machine.ready(100);
    machine.fail(200);
    machine.tick(2399);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(InteractionState::error),
        static_cast<int>(machine.state()));
    machine.tick(2400);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(InteractionState::idle),
        static_cast<int>(machine.state()));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_short_press_requests_sleep);
    RUN_TEST(test_hold_records_until_release);
    RUN_TEST(test_release_at_hold_threshold_without_tick_requests_sleep);
    RUN_TEST(test_inactivity_sleeps_only_from_idle);
    RUN_TEST(test_inactivity_handles_millisecond_wrap);
    RUN_TEST(test_busy_flow_returns_to_fresh_idle_timer);
    RUN_TEST(test_error_returns_to_idle_after_visible_period);
    return UNITY_END();
}
