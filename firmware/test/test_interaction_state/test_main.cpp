#include <cstdint>
#include <cstring>
#include <limits>

#include <unity.h>

#include "chatesp/app_mode.hpp"
#include "chatesp/button_debouncer.hpp"
#include "chatesp/ble_shutdown.hpp"
#include "chatesp/interaction_state.hpp"
#include "chatesp/power_button_filter.hpp"
#include "chatesp/quick_controls.hpp"
#include "chatesp/user_error_message.hpp"

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

void test_unconfirmed_or_electrically_short_press_stays_awake() {
    InteractionStateMachine machine;
    machine.ready(1'000);
    machine.button_down(1'100);
    machine.button_up(1'300, false);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(InteractionState::idle),
        static_cast<int>(machine.state()));

    machine.button_down(1'400);
    machine.button_up(1'479, true);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(InteractionState::idle),
        static_cast<int>(machine.state()));

    machine.button_down(1'500);
    machine.button_up(1'580, true);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(InteractionState::sleep_pending),
        static_cast<int>(machine.state()));
}

void test_ble_shutdown_retries_each_incomplete_step() {
    chatesp::runtime::BleShutdown shutdown;
    using Step = chatesp::runtime::BleShutdown::Step;

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(Step::stop_host),
        static_cast<int>(shutdown.step()));
    shutdown.host_stopped();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(Step::deinitialize_host),
        static_cast<int>(shutdown.step()));
    shutdown.host_deinitialized();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(Step::complete),
        static_cast<int>(shutdown.step()));
    shutdown.reset();
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(Step::stop_host),
        static_cast<int>(shutdown.step()));
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

void test_request_errors_have_explicit_user_messages() {
    TEST_ASSERT_EQUAL_STRING(
        "THE SERVICE RETURNED INVALID DATA",
        chatesp::request_error_message(
            chatesp::agent::Error::malformed_response));
    TEST_ASSERT_EQUAL_STRING(
        "THE MODEL COULD NOT COMPLETE THE ANSWER",
        chatesp::request_error_message(chatesp::agent::Error::model_failed));

    for (unsigned value = 0;
         value <= static_cast<unsigned>(chatesp::agent::Error::model_failed);
         ++value) {
        const char *message = chatesp::request_error_message(
            static_cast<chatesp::agent::Error>(value));
        TEST_ASSERT_NOT_NULL(message);
        TEST_ASSERT_GREATER_THAN_UINT32(0, std::strlen(message));
        TEST_ASSERT_NULL(std::strstr(message, "TRY AGAIN"));
        TEST_ASSERT_NULL(std::strstr(message, "WATCH"));
        for (const char *cursor = message; *cursor != '\0'; ++cursor) {
            TEST_ASSERT_FALSE(*cursor >= '0' && *cursor <= '9');
        }
    }
}

void test_speech_errors_have_explicit_user_messages() {
    TEST_ASSERT_EQUAL_STRING(
        "UNABLE TO PLAY THE ANSWER",
        chatesp::speech_error_message(chatesp::agent::Error::model_failed));
    TEST_ASSERT_EQUAL_STRING(
        "THE SPEECH SERVICE RETURNED INVALID AUDIO",
        chatesp::speech_error_message(
            chatesp::agent::Error::unsupported_media));

    for (unsigned value = 0;
         value <= static_cast<unsigned>(chatesp::agent::Error::model_failed);
         ++value) {
        const char *message = chatesp::speech_error_message(
            static_cast<chatesp::agent::Error>(value));
        TEST_ASSERT_NOT_NULL(message);
        TEST_ASSERT_GREATER_THAN_UINT32(0, std::strlen(message));
        TEST_ASSERT_NULL(std::strstr(message, "WATCH"));
        for (const char *cursor = message; *cursor != '\0'; ++cursor) {
            TEST_ASSERT_FALSE(*cursor >= '0' && *cursor <= '9');
        }
    }
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

void test_power_button_filter_rejects_usb_source_transitions() {
    chatesp::PowerButtonFilter button{30};
    button.reset(false, 100);

    TEST_ASSERT_FALSE(button.update(true, false, true, 110).pressed);
    TEST_ASSERT_FALSE(button.update(false, false, false, 500).pressed);
}

void test_power_button_filter_accepts_pmu_edges() {
    chatesp::PowerButtonFilter button{30};
    button.reset(false, 100);

    TEST_ASSERT_FALSE(button.update(true, false, false, 110).pressed);
    TEST_ASSERT_FALSE(button.update(false, false, false, 139).pressed);
    TEST_ASSERT_TRUE(button.update(false, false, false, 140).pressed);
    TEST_ASSERT_FALSE(button.update(false, true, false, 500).released);
    TEST_ASSERT_TRUE(button.update(false, false, false, 530).released);
}

void test_power_button_filter_confirms_only_a_pmu_short_press() {
    chatesp::PowerButtonFilter button{30};
    button.reset(false, 100);

    TEST_ASSERT_FALSE(button.update(true, false, false, false, 110).pressed);
    TEST_ASSERT_TRUE(button.update(false, false, false, false, 140).pressed);
    TEST_ASSERT_FALSE(button.update(false, true, false, true, 500).released);
    const chatesp::ButtonEdges confirmed =
        button.update(false, false, false, false, 530);
    TEST_ASSERT_TRUE(confirmed.released);
    TEST_ASSERT_TRUE(confirmed.short_press_confirmed);

    button.reset(false, 600);
    TEST_ASSERT_FALSE(button.update(true, false, false, false, 610).pressed);
    TEST_ASSERT_TRUE(button.update(false, false, false, false, 640).pressed);
    TEST_ASSERT_FALSE(button.update(false, true, false, false, 700).released);
    const chatesp::ButtonEdges unconfirmed =
        button.update(false, false, false, false, 730);
    TEST_ASSERT_TRUE(unconfirmed.released);
    TEST_ASSERT_FALSE(unconfirmed.short_press_confirmed);
}

void test_power_button_filter_quarantines_adjacent_usb_press_noise() {
    chatesp::PowerButtonFilter button{30, 250};
    button.reset(false, 100);

    TEST_ASSERT_FALSE(button.update(false, false, true, false, 200).pressed);
    TEST_ASSERT_FALSE(button.update(true, false, false, false, 215).pressed);
    TEST_ASSERT_FALSE(button.update(false, false, false, false, 500).pressed);

    TEST_ASSERT_FALSE(button.update(true, false, false, false, 500).pressed);
    TEST_ASSERT_TRUE(button.update(false, false, false, false, 530).pressed);
}

void test_power_button_filter_accepts_release_without_an_exio_change() {
    chatesp::PowerButtonFilter button{30};
    button.reset(true, 100);

    TEST_ASSERT_FALSE(button.update(false, true, false, 110).released);
    TEST_ASSERT_FALSE(button.update(false, false, false, 139).released);
    TEST_ASSERT_TRUE(button.update(false, false, false, 140).released);
}

void test_power_button_filter_keeps_a_press_across_usb_removal() {
    chatesp::PowerButtonFilter button{30};
    button.reset(false, 100);

    TEST_ASSERT_FALSE(button.update(true, false, false, 110).pressed);
    TEST_ASSERT_TRUE(button.update(false, false, false, 140).pressed);
    TEST_ASSERT_FALSE(button.update(false, false, true, 200).released);
    TEST_ASSERT_FALSE(button.update(false, true, false, 300).released);
    TEST_ASSERT_TRUE(button.update(false, false, false, 330).released);
}

void test_power_button_filter_rejects_key_edges_with_a_usb_event() {
    chatesp::PowerButtonFilter button{30};
    button.reset(false, 100);

    TEST_ASSERT_FALSE(button.update(true, false, true, 110).pressed);
    TEST_ASSERT_FALSE(button.update(false, false, false, 500).pressed);

    button.reset(true, 600);
    TEST_ASSERT_FALSE(button.update(false, true, true, 610).released);
    TEST_ASSERT_FALSE(button.update(false, false, false, 1'000).released);
}

void test_power_button_filter_ignores_two_key_edges_in_one_poll() {
    chatesp::PowerButtonFilter button{30};
    button.reset(false, 100);

    TEST_ASSERT_FALSE(button.update(true, true, false, 110).pressed);
    TEST_ASSERT_FALSE(button.update(false, false, false, 500).pressed);
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

void test_quick_controls_open_only_from_top_and_continues_below_target() {
    chatesp::QuickControlsGesture controls;
    controls.set_allowed(true);

    controls.press(180, 80, 100);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::QuickControlsAction::none),
        static_cast<int>(controls.release(180, 180, 200)));

    controls.press(180, 20, 300);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::QuickControlsAction::open),
        static_cast<int>(controls.release(180, 220, 400)));
}

void test_quick_controls_reject_a_sideways_drag() {
    chatesp::QuickControlsGesture controls;
    controls.set_allowed(true);
    controls.press(40, 20, 100);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::QuickControlsAction::none),
        static_cast<int>(controls.release(140, 100, 200)));
    TEST_ASSERT_FALSE(controls.release_was_accepted());
}

void test_quick_controls_reports_live_finger_distance() {
    chatesp::QuickControlsGesture controls;
    controls.set_allowed(true);
    controls.press(180, 20, 100);
    TEST_ASSERT_TRUE(controls.pressed());
    TEST_ASSERT_EQUAL_INT32(0, controls.drag_distance_y(20));
    TEST_ASSERT_EQUAL_INT32(73, controls.drag_distance_y(93));
    controls.release(180, 93, 200);
    TEST_ASSERT_FALSE(controls.pressed());
    TEST_ASSERT_TRUE(controls.release_was_accepted());
    TEST_ASSERT_EQUAL_INT32(0, controls.drag_distance_y(120));
}

void test_quick_controls_close_with_an_upward_swipe() {
    chatesp::QuickControlsGesture controls;
    controls.set_allowed(true);
    controls.set_open(true, 100);
    controls.press(180, 220, 200);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::QuickControlsAction::close),
        static_cast<int>(controls.release(180, 172, 300)));
}

void test_quick_controls_settle_at_half_deployment() {
    constexpr std::int32_t hidden_y = -288;
    constexpr std::int32_t shown_y = -12;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::QuickControlsAction::close),
        static_cast<int>(chatesp::quick_controls_settle_action(
            -151, hidden_y, shown_y)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::QuickControlsAction::open),
        static_cast<int>(chatesp::quick_controls_settle_action(
            -149, hidden_y, shown_y)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::QuickControlsAction::open),
        static_cast<int>(chatesp::quick_controls_settle_action(
            -150, hidden_y, shown_y)));
}

void test_quick_controls_auto_close_handles_wrap_and_active_touch() {
    chatesp::QuickControlsGesture controls;
    controls.set_allowed(true);
    const std::uint32_t start =
        std::numeric_limits<std::uint32_t>::max() - 2'000;
    controls.set_open(true, start);
    TEST_ASSERT_FALSE(controls.automatic_close_due(2'998));
    controls.press(180, 100, 2'999);
    TEST_ASSERT_FALSE(controls.automatic_close_due(20'000));
    controls.release(180, 100, 3'000);
    TEST_ASSERT_TRUE(controls.automatic_close_due(8'000));
}

void test_quick_controls_snap_to_valid_five_percent_steps() {
    TEST_ASSERT_EQUAL_UINT8(
        5, chatesp::QuickControlsGesture::snap_percent(0, 5));
    TEST_ASSERT_EQUAL_UINT8(
        65, chatesp::QuickControlsGesture::snap_percent(63, 5));
    TEST_ASSERT_EQUAL_UINT8(
        0, chatesp::QuickControlsGesture::snap_percent(1, 0));
    TEST_ASSERT_EQUAL_UINT8(
        100, chatesp::QuickControlsGesture::snap_percent(104, 0));
    TEST_ASSERT_EQUAL_UINT8(
        0,
        chatesp::QuickControlsGesture::percent_for_track_position(
            0, 288, 0));
    TEST_ASSERT_EQUAL_UINT8(
        0,
        chatesp::QuickControlsGesture::percent_for_track_position(
            -32, 288, 0));
    TEST_ASSERT_EQUAL_UINT8(
        50,
        chatesp::QuickControlsGesture::percent_for_track_position(
            144, 288, 0));
    TEST_ASSERT_EQUAL_UINT8(
        100,
        chatesp::QuickControlsGesture::percent_for_track_position(
            319, 288, 0));
    TEST_ASSERT_EQUAL_UINT8(
        55,
        chatesp::QuickControlsGesture::percent_for_track_position(
            144, 288, 5));
    TEST_ASSERT_EQUAL_UINT8(
        5,
        chatesp::QuickControlsGesture::percent_for_track_position(
            -32, 288, 5));
}

void test_quick_controls_defer_flash_work_until_input_is_idle() {
    TEST_ASSERT_TRUE(
        chatesp::quick_controls_can_persist(false, false, false));
    TEST_ASSERT_FALSE(
        chatesp::quick_controls_can_persist(true, false, false));
    TEST_ASSERT_FALSE(
        chatesp::quick_controls_can_persist(false, true, false));
    TEST_ASSERT_FALSE(
        chatesp::quick_controls_can_persist(false, false, true));
    TEST_ASSERT_FALSE(
        chatesp::quick_controls_can_persist(true, true, true));
}

void test_mode_button_accepts_only_a_short_complete_press() {
    chatesp::ShortPressGesture button{700, 80};
    TEST_ASSERT_FALSE(button.release(10));
    button.press(100);
    TEST_ASSERT_TRUE(button.release(800));
    button.press(900);
    TEST_ASSERT_FALSE(button.release(979));
    button.press(1'000);
    TEST_ASSERT_TRUE(button.release(1'080));
    button.press(2'000);
    TEST_ASSERT_FALSE(button.release(2'701));
    button.press(std::numeric_limits<std::uint32_t>::max() - 99);
    TEST_ASSERT_TRUE(button.release(500));
    button.press(1'000);
    button.cancel();
    TEST_ASSERT_FALSE(button.release(1'010));
}

void test_clock_time_acquisition_has_a_bounded_network_window() {
    TEST_ASSERT_FALSE(chatesp::clock_network_shutdown_due(
        false, true, 0, 15'000));
    TEST_ASSERT_FALSE(chatesp::clock_network_shutdown_due(
        true, false, 14'999, 15'000));
    TEST_ASSERT_TRUE(chatesp::clock_network_shutdown_due(
        true, true, 1, 15'000));
    TEST_ASSERT_TRUE(chatesp::clock_network_shutdown_due(
        true, false, 15'000, 15'000));
    TEST_ASSERT_TRUE(chatesp::clock_network_shutdown_due(
        true, false, 200, 100));
}

void test_clock_path_changes_one_pixel_at_a_time() {
    constexpr std::uint16_t point_count = 1'500;
    chatesp::ClockPathSpan span =
        chatesp::clock_path_span(2, 0, point_count);
    TEST_ASSERT_EQUAL_UINT16(0, span.first);
    TEST_ASSERT_EQUAL_UINT16(0, span.count);

    span = chatesp::clock_path_span(2, 40, point_count);
    TEST_ASSERT_EQUAL_UINT16(1, span.count);
    TEST_ASSERT_TRUE(chatesp::clock_path_point_visible(0, span));
    TEST_ASSERT_FALSE(chatesp::clock_path_point_visible(1, span));

    span = chatesp::clock_path_span(2, 59'999, point_count);
    TEST_ASSERT_EQUAL_UINT16(point_count - 1, span.count);
    TEST_ASSERT_TRUE(chatesp::clock_path_point_visible(
        point_count - 2, span));

    span = chatesp::clock_path_span(3, 0, point_count);
    TEST_ASSERT_EQUAL_UINT16(0, span.first);
    TEST_ASSERT_EQUAL_UINT16(point_count, span.count);

    span = chatesp::clock_path_span(3, 40, point_count);
    TEST_ASSERT_EQUAL_UINT16(1, span.first);
    TEST_ASSERT_EQUAL_UINT16(point_count - 1, span.count);
    TEST_ASSERT_FALSE(chatesp::clock_path_point_visible(0, span));
    TEST_ASSERT_TRUE(chatesp::clock_path_point_visible(1, span));

    span = chatesp::clock_path_span(3, 60'000, point_count);
    TEST_ASSERT_EQUAL_UINT16(point_count - 1, span.first);
    TEST_ASSERT_EQUAL_UINT16(1, span.count);
}

void test_clock_configuration_and_time_text_are_bounded() {
    TEST_ASSERT_TRUE(chatesp::ClockStyle{}.valid());
    chatesp::ClockStyle invalid;
    invalid.corner_radius_px = 121;
    TEST_ASSERT_FALSE(invalid.valid());

    const auto available = chatesp::clock_time_text(
        true, chatesp::ClockTime{23, 59, 59});
    TEST_ASSERT_EQUAL_STRING("23:59", available.data());
    const auto midnight = chatesp::clock_time_text(
        true, chatesp::ClockTime{0, 0, 0});
    TEST_ASSERT_EQUAL_STRING("00:00", midnight.data());
    const auto unavailable = chatesp::clock_time_text(false, {});
    TEST_ASSERT_EQUAL_STRING("--:--", unavailable.data());
    const auto invalid_time = chatesp::clock_time_text(
        true, chatesp::ClockTime{24, 0, 0});
    TEST_ASSERT_EQUAL_STRING("--:--", invalid_time.data());
    const auto invalid_millisecond = chatesp::clock_time_text(
        true, chatesp::ClockTime{12, 0, 0, 1'000});
    TEST_ASSERT_EQUAL_STRING("--:--", invalid_millisecond.data());
}

void test_pairing_code_always_uses_chat_orientation() {
    using chatesp::AppMode;
    using chatesp::DisplayOrientation;

    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(DisplayOrientation::chat),
        static_cast<int>(chatesp::display_orientation_for(
            AppMode::chat, false)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(DisplayOrientation::clock),
        static_cast<int>(chatesp::display_orientation_for(
            AppMode::clock, false)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(DisplayOrientation::chat),
        static_cast<int>(chatesp::display_orientation_for(
            AppMode::chat, true)));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(DisplayOrientation::chat),
        static_cast<int>(chatesp::display_orientation_for(
            AppMode::clock, true)));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_short_press_requests_sleep);
    RUN_TEST(test_unconfirmed_or_electrically_short_press_stays_awake);
    RUN_TEST(test_ble_shutdown_retries_each_incomplete_step);
    RUN_TEST(test_hold_records_until_release);
    RUN_TEST(test_release_at_hold_threshold_without_tick_requests_sleep);
    RUN_TEST(test_inactivity_sleeps_only_from_idle);
    RUN_TEST(test_inactivity_handles_millisecond_wrap);
    RUN_TEST(test_idle_activity_extends_the_sleep_timer);
    RUN_TEST(test_busy_flow_returns_to_fresh_idle_timer);
    RUN_TEST(test_error_returns_to_idle_after_visible_period);
    RUN_TEST(test_request_errors_have_explicit_user_messages);
    RUN_TEST(test_speech_errors_have_explicit_user_messages);
    RUN_TEST(test_button_hold_preempts_active_work);
    RUN_TEST(test_button_debouncer_rejects_bounce_and_handles_wrap);
    RUN_TEST(test_power_button_filter_rejects_usb_source_transitions);
    RUN_TEST(test_power_button_filter_accepts_pmu_edges);
    RUN_TEST(test_power_button_filter_confirms_only_a_pmu_short_press);
    RUN_TEST(test_power_button_filter_quarantines_adjacent_usb_press_noise);
    RUN_TEST(test_power_button_filter_accepts_release_without_an_exio_change);
    RUN_TEST(test_power_button_filter_keeps_a_press_across_usb_removal);
    RUN_TEST(test_power_button_filter_rejects_key_edges_with_a_usb_event);
    RUN_TEST(test_power_button_filter_ignores_two_key_edges_in_one_poll);
    RUN_TEST(test_short_wake_press_returns_to_idle);
    RUN_TEST(test_held_wake_press_records_until_release);
    RUN_TEST(test_quick_controls_open_only_from_top_and_continues_below_target);
    RUN_TEST(test_quick_controls_reject_a_sideways_drag);
    RUN_TEST(test_quick_controls_reports_live_finger_distance);
    RUN_TEST(test_quick_controls_close_with_an_upward_swipe);
    RUN_TEST(test_quick_controls_settle_at_half_deployment);
    RUN_TEST(test_quick_controls_auto_close_handles_wrap_and_active_touch);
    RUN_TEST(test_quick_controls_snap_to_valid_five_percent_steps);
    RUN_TEST(test_quick_controls_defer_flash_work_until_input_is_idle);
    RUN_TEST(test_mode_button_accepts_only_a_short_complete_press);
    RUN_TEST(test_clock_time_acquisition_has_a_bounded_network_window);
    RUN_TEST(test_clock_path_changes_one_pixel_at_a_time);
    RUN_TEST(test_clock_configuration_and_time_text_are_bounded);
    RUN_TEST(test_pairing_code_always_uses_chat_orientation);
    return UNITY_END();
}
