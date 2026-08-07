#include <cstdint>
#include <limits>

#include <unity.h>

#include "chatesp/button_debouncer.hpp"
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

void test_idle_activity_extends_the_sleep_timer() {
    InteractionStateMachine machine;
    machine.ready(100);
    machine.note_idle_activity(20'000);
    machine.tick(49'999);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(InteractionState::idle),
        static_cast<int>(machine.state()));
    machine.tick(50'000);
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

void test_button_hold_preempts_active_work() {
    InteractionStateMachine machine;
    machine.ready(0);
    machine.button_down(1);
    machine.tick(351);
    machine.button_up(400);
    machine.transcription_ready(500);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(InteractionState::thinking),
        static_cast<int>(machine.state()));

    machine.button_down(600);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(InteractionState::idle),
        static_cast<int>(machine.state()));
    machine.tick(950);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(InteractionState::recording),
        static_cast<int>(machine.state()));
}

void test_button_debouncer_rejects_bounce_and_handles_wrap() {
    chatesp::ButtonDebouncer button{30};
    button.reset(false, 100);
    TEST_ASSERT_FALSE(button.update(true, 110).pressed);
    TEST_ASSERT_FALSE(button.update(false, 120).pressed);
    TEST_ASSERT_FALSE(button.update(true, 130).pressed);
    TEST_ASSERT_FALSE(button.update(true, 159).pressed);
    TEST_ASSERT_TRUE(button.update(true, 160).pressed);

    const std::uint32_t near_wrap =
        std::numeric_limits<std::uint32_t>::max() - 9;
    button.reset(true, near_wrap);
    TEST_ASSERT_FALSE(button.update(false, near_wrap).released);
    TEST_ASSERT_FALSE(button.update(false, 19).released);
    TEST_ASSERT_TRUE(button.update(false, 20).released);
}

void test_short_wake_press_returns_to_idle() {
    InteractionStateMachine machine;
    machine.ready(100);
    machine.wake_button_down(110);
    machine.button_up(250);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(InteractionState::idle),
        static_cast<int>(machine.state()));
}

void test_held_wake_press_records_until_release() {
    InteractionStateMachine machine;
    machine.ready(100);
    machine.wake_button_down(110);
    machine.tick(460);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(InteractionState::recording),
        static_cast<int>(machine.state()));
    machine.button_up(1000);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(InteractionState::transcribing),
        static_cast<int>(machine.state()));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_short_press_requests_sleep);
    RUN_TEST(test_hold_records_until_release);
    RUN_TEST(test_release_at_hold_threshold_without_tick_requests_sleep);
    RUN_TEST(test_inactivity_sleeps_only_from_idle);
    RUN_TEST(test_inactivity_handles_millisecond_wrap);
    RUN_TEST(test_idle_activity_extends_the_sleep_timer);
    RUN_TEST(test_busy_flow_returns_to_fresh_idle_timer);
    RUN_TEST(test_error_returns_to_idle_after_visible_period);
    RUN_TEST(test_button_hold_preempts_active_work);
    RUN_TEST(test_button_debouncer_rejects_bounce_and_handles_wrap);
    RUN_TEST(test_short_wake_press_returns_to_idle);
    RUN_TEST(test_held_wake_press_records_until_release);
    return UNITY_END();
}
