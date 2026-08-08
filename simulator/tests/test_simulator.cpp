#include "chatesp/simulator/simulator.hpp"
#include "chatesp/simulator/svg_renderer.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using chatesp::AppMode;
using chatesp::DisplayOrientation;
using chatesp::InteractionState;
using chatesp::simulator::Simulator;

int failures = 0;

void check(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void start_recording(Simulator &simulator) {
    check(simulator.ready(), "ready must accept one boot transition");
    check(simulator.action_button(true), "action down must be accepted");
    check(simulator.advance(350), "recording advance must be accepted");
    check(
        simulator.snapshot().interaction == InteractionState::recording,
        "hold threshold must start recording");
    check(simulator.action_button(false), "action up must be accepted");
    check(
        simulator.snapshot().interaction == InteractionState::transcribing,
        "recording release must start transcription");
}

void test_short_press_sleeps_and_clears_private_text() {
    Simulator simulator;
    check(simulator.ready(), "ready transition failed");
    check(simulator.action_button(true), "short press down failed");
    check(simulator.advance(100), "short-press advance must be accepted");
    check(simulator.action_button(false), "short press up failed");
    const auto state = simulator.snapshot();
    check(
        state.interaction == InteractionState::sleep_pending,
        "short press must request sleep");
    check(!state.screen_on, "sleep must turn the simulated display off");
    check(state.transcript_bytes == 0, "sleep must clear the transcript");
    check(state.answer_bytes == 0, "sleep must clear the answer");
}

void test_voice_flow_returns_to_clock() {
    Simulator simulator;
    start_recording(simulator);
    check(
        simulator.set_transcript("synthetic transcript"),
        "transcript must start model work");
    check(
        simulator.snapshot().interaction == InteractionState::thinking,
        "transcript must enter thinking");
    check(simulator.start_tool(), "tool start must be accepted");
    check(
        simulator.set_answer("synthetic answer"),
        "answer must start speech");
    check(
        simulator.snapshot().interaction == InteractionState::speaking,
        "answer must enter speaking");
    check(simulator.finish_interaction(), "finish must be accepted");
    check(simulator.advance(29'999), "follow-up advance must be accepted");
    check(
        simulator.snapshot().mode == AppMode::chat,
        "Clock return must wait for the full follow-up interval");
    check(simulator.advance(1), "Clock boundary advance must be accepted");
    const auto state = simulator.snapshot();
    check(state.mode == AppMode::clock, "voice flow must return to Clock");
    check(
        state.orientation == DisplayOrientation::clock,
        "Clock must use the landscape orientation");
    check(state.transcript_bytes == 0, "Clock entry must clear transcript");
    check(state.answer_bytes == 0, "Clock entry must clear answer");
}

void test_mode_and_pairing_orientation() {
    Simulator simulator;
    check(simulator.ready(), "ready transition failed");
    check(simulator.mode_button(79), "short electrical pulse must be valid");
    check(
        simulator.snapshot().mode == AppMode::chat,
        "short electrical pulse must not change mode");
    check(simulator.mode_button(80), "minimum top-button press must work");
    check(
        simulator.snapshot().orientation == DisplayOrientation::clock,
        "Clock must use landscape orientation");
    check(
        simulator.show_pairing_code(42),
        "bounded pairing code must be accepted");
    check(
        simulator.snapshot().orientation == DisplayOrientation::chat,
        "pairing must use chat orientation");
    simulator.hide_pairing_code();
    check(
        simulator.snapshot().orientation == DisplayOrientation::clock,
        "pairing close must restore Clock orientation");
    check(simulator.mode_button(701), "long top-button press must be valid");
    check(
        simulator.snapshot().mode == AppMode::clock,
        "long top-button press must not change mode");
    check(
        !simulator.show_pairing_code(1'000'000),
        "pairing code must have six digits at most");
}

void test_touch_controls_are_bounded() {
    Simulator simulator;
    check(simulator.ready(), "ready transition failed");
    check(simulator.touch_down(184, 10), "top touch must start a gesture");
    check(simulator.touch_up(184, 70), "top swipe must complete");
    check(simulator.snapshot().controls_open, "down swipe must open controls");
    check(simulator.set_brightness(58), "brightness update must work");
    check(simulator.set_volume(2), "volume update must work");
    check(
        simulator.snapshot().brightness_percent == 60,
        "brightness must snap to five percent");
    check(
        simulator.snapshot().volume_percent == 0,
        "volume must snap to zero percent");
    check(!simulator.set_brightness(4), "brightness below five must fail");
    check(!simulator.set_volume(101), "volume above 100 must fail");
    check(simulator.advance(5'000), "control timeout advance must be accepted");
    check(
        !simulator.snapshot().controls_open,
        "controls must close after five seconds");
}

void test_status_is_private_and_svg_is_explicit() {
    Simulator simulator;
    start_recording(simulator);
    constexpr char transcript[] = "PRIVATE_SYNTHETIC_TRANSCRIPT";
    constexpr char answer[] = "PRIVATE_SYNTHETIC_ANSWER";
    check(simulator.set_transcript(transcript), "transcript failed");
    check(simulator.set_answer(answer), "answer failed");
    const std::string status = simulator.status_json();
    check(
        status.find(transcript) == std::string::npos,
        "status must not contain transcript text");
    check(
        status.find(answer) == std::string::npos,
        "status must not contain answer text");
    check(
        status.find("transcript_bytes") != std::string::npos,
        "status must contain transcript metadata");

    const std::string svg =
        chatesp::simulator::render_svg_text(simulator.display_view());
    check(
        svg.find(transcript) != std::string::npos,
        "explicit display artifact must contain transcript text");
    check(
        svg.find(answer) != std::string::npos,
        "explicit display artifact must contain answer text");
    check(
        svg.find("width=\"368\"") != std::string::npos,
        "chat artifact must use the physical display width");
}

void test_invalid_text_and_wake_paths() {
    Simulator simulator;
    start_recording(simulator);
    std::string too_large(
        chatesp::simulator::kMaximumTranscriptBytes + 1, 'x');
    check(!simulator.set_transcript(""), "empty transcript must fail");
    check(
        !simulator.set_transcript(too_large),
        "oversized transcript must fail");
    const std::string control_text{"bad\x01text", 8};
    check(
        !simulator.set_transcript(control_text),
        "control characters must fail");
    const std::string invalid_utf8{"\xc0\xaf", 2};
    check(!simulator.set_transcript(invalid_utf8), "invalid UTF-8 must fail");
    check(
        simulator.set_transcript("Question française"),
        "valid UTF-8 must be accepted");
    check(simulator.fail_interaction(), "failure must be accepted");
    check(simulator.advance(2'200), "error timeout advance must be accepted");
    check(
        simulator.snapshot().interaction == InteractionState::idle,
        "visible error must recover to idle");
    check(simulator.action_button(true), "short press down failed");
    check(simulator.advance(100), "sleep advance must be accepted");
    check(simulator.action_button(false), "short press up failed");
    check(!simulator.snapshot().screen_on, "short press must sleep");
    check(simulator.action_button(true), "sleep wake press failed");
    check(simulator.advance(100), "wake advance must be accepted");
    check(simulator.action_button(false), "sleep wake release failed");
    check(simulator.snapshot().screen_on, "wake must restore display");
    check(
        simulator.snapshot().interaction == InteractionState::idle,
        "short wake must return to idle");
}

void test_answer_limit_and_clock_network_window() {
    Simulator simulator;
    start_recording(simulator);
    check(simulator.set_transcript("question"), "transcript failed");
    std::string large_answer(
        chatesp::simulator::kMaximumAnswerBytes + 1, 'x');
    check(!simulator.set_answer(large_answer), "oversized answer must fail");
    check(simulator.set_answer("answer"), "bounded answer must work");
    check(simulator.finish_interaction(), "finish must work");
    check(simulator.set_clock_time(false), "unavailable time must work");
    check(simulator.mode_button(100), "Clock entry must work");
    check(
        simulator.snapshot().clock_network_shutdown_pending,
        "Clock must keep its bounded time network window");
    check(simulator.advance(14'999), "Clock network advance must work");
    check(
        simulator.snapshot().wifi != chatesp::simulator::WifiState::off,
        "Clock network must stay available before the limit");
    check(simulator.advance(1), "Clock network boundary must work");
    check(
        simulator.snapshot().wifi == chatesp::simulator::WifiState::off,
        "Clock network must stop at the limit");

    check(simulator.mode_button(100), "Chat entry must work");
    simulator.set_wifi(chatesp::simulator::WifiState::online);
    check(
        simulator.set_clock_time(true, chatesp::ClockTime{12, 34, 56}),
        "available Clock time must work");
    check(simulator.mode_button(100), "second Clock entry must work");
    check(
        simulator.snapshot().wifi == chatesp::simulator::WifiState::off,
        "Clock must stop Wi-Fi at once when time is ready");
}

void test_large_advance_is_bounded_and_keeps_intermediate_time() {
    Simulator simulator;
    check(simulator.ready(), "ready transition failed");
    check(simulator.fail_interaction(), "failure must be accepted");
    check(
        simulator.advance(32'200),
        "bounded large advance must be accepted");
    check(
        simulator.snapshot().interaction == InteractionState::sleep_pending,
        "large advance must process error recovery before idle sleep");
    const std::uint32_t prior_time = simulator.snapshot().now_ms;
    check(
        !simulator.advance(chatesp::simulator::kMaximumAdvanceMs + 1),
        "unbounded advance must fail");
    check(
        simulator.snapshot().now_ms == prior_time,
        "failed advance must not change time");
}

void test_ble_pairing_security_and_retry_flow() {
    Simulator simulator(true);
    check(simulator.ready(), "BLE simulator must become ready");
    check(simulator.ble_connect(), "BLE connection must start pairing");
    check(
        simulator.snapshot().ble.state ==
            chatesp::simulator::BleState::pairing,
        "unbonded BLE connection must require pairing");
    check(
        simulator.snapshot().pairing_code_visible,
        "BLE pairing must show the passkey on the simulated display");
    check(
        simulator.mode_button(100),
        "mode change during BLE pairing must be processed");
    check(
        simulator.snapshot().pairing_code_visible,
        "Clock entry must not hide an active BLE passkey");
    check(
        simulator.snapshot().orientation == DisplayOrientation::chat,
        "active BLE pairing must keep the portrait orientation");
    check(
        simulator.ble_provision(1),
        "insecure provisioning attempt must be processed");
    check(
        simulator.snapshot().ble.outcome ==
            chatesp::simulator::BleOutcome::authentication_required,
        "insecure provisioning must be rejected");
    check(
        simulator.snapshot().ble.storage_writes == 0,
        "insecure provisioning must not write settings");
    check(
        simulator.ble_confirm_pairing(123'456),
        "matching passkey must complete pairing");
    check(simulator.snapshot().ble.secure, "paired link must be secure");
    check(simulator.snapshot().ble.bonded, "paired link must be bonded");
    check(
        !simulator.snapshot().pairing_code_visible,
        "completed pairing must hide the passkey");
    check(
        simulator.snapshot().orientation == DisplayOrientation::clock,
        "pairing completion must restore the Clock orientation");
    check(simulator.ble_provision(1), "secure provisioning must run");
    check(
        simulator.snapshot().ble.outcome ==
            chatesp::simulator::BleOutcome::applied,
        "first secure provisioning must apply");
    check(
        simulator.snapshot().ble.storage_writes == 1,
        "first secure provisioning must write once");

    check(
        simulator.ble_provision(
            2, chatesp::simulator::BleFault::drop_acknowledgement),
        "dropped acknowledgement simulation must run");
    check(
        simulator.snapshot().ble.outcome ==
            chatesp::simulator::BleOutcome::unchanged,
        "retry after a dropped acknowledgement must return unchanged");
    check(
        simulator.snapshot().ble.attempts == 2,
        "dropped acknowledgement must cause one bounded retry");
    check(
        simulator.snapshot().ble.state ==
            chatesp::simulator::BleState::connected,
        "acknowledgement retry must keep the secure connection");
    check(
        simulator.snapshot().ble.storage_writes == 2,
        "retry must not cause a second settings write");
}

void test_ble_faults_recover_without_partial_state() {
    Simulator simulator(true);
    check(simulator.ready(), "BLE fault simulator must become ready");
    check(simulator.ble_connect(), "BLE fault connection must start");
    check(simulator.ble_confirm_pairing(123'456), "BLE fault pairing failed");
    check(simulator.ble_provision(1), "initial BLE settings must apply");

    check(
        simulator.ble_provision(
            2, chatesp::simulator::BleFault::corrupt_data),
        "corrupt BLE transfer must be processed");
    check(
        simulator.snapshot().ble.outcome ==
            chatesp::simulator::BleOutcome::malformed_transfer,
        "corrupt BLE data must be rejected as a malformed transfer");
    check(
        simulator.snapshot().ble.active_revision == 1,
        "corrupt BLE data must keep the old revision");

    check(
        simulator.ble_provision(
            2, chatesp::simulator::BleFault::storage_failure),
        "BLE storage failure must be processed");
    check(
        simulator.snapshot().ble.outcome ==
            chatesp::simulator::BleOutcome::storage_failure,
        "storage failure must not report applied");
    check(
        simulator.snapshot().ble.active_revision == 1,
        "storage failure must keep the old revision");

    check(
        simulator.ble_provision(
            2, chatesp::simulator::BleFault::disconnect_after_data),
        "mid-transfer disconnect must be processed");
    check(
        simulator.snapshot().ble.outcome ==
            chatesp::simulator::BleOutcome::disconnected,
        "mid-transfer disconnect must fail the active iOS request");
    check(
        simulator.snapshot().ble.attempts == 1,
        "mid-transfer disconnect must not retry the iOS request");
    check(
        simulator.snapshot().ble.storage_writes == 1,
        "mid-transfer disconnect must not write partial settings");
    check(
        simulator.ble_connect(),
        "bonded device must reconnect after a transfer disconnect");
    check(
        simulator.ble_provision(2),
        "a new request after reconnect must apply");
    check(
        simulator.snapshot().ble.active_revision == 2,
        "a new request must apply only after reconnect");
}

void test_ble_bond_restart_and_sanitized_fuzz() {
    Simulator rejected(true);
    check(rejected.ready(), "rejected pairing simulator must be ready");
    check(rejected.ble_connect(), "rejected pairing must connect");
    check(rejected.ble_reject_pairing(), "pairing rejection must run");
    check(
        rejected.snapshot().ble.outcome ==
            chatesp::simulator::BleOutcome::pairing_failed,
        "pairing rejection must report failure");
    check(
        rejected.snapshot().ble.state ==
            chatesp::simulator::BleState::advertising,
        "pairing rejection must return to advertising");
    check(rejected.ble_connect(), "second pairing attempt must connect");
    check(
        rejected.ble_confirm_pairing(123'456),
        "wrong passkey must be processed");
    check(
        rejected.snapshot().ble.outcome ==
            chatesp::simulator::BleOutcome::pairing_failed,
        "wrong passkey must fail pairing");

    Simulator development(true);
    check(development.ready(), "development BLE simulator must be ready");
    check(development.ble_connect(), "development BLE connect failed");
    check(
        development.ble_confirm_pairing(123'456),
        "development BLE pairing failed");
    development.ble_restart_radio();
    check(
        development.ble_connect(),
        "development BLE radio restart must reconnect");
    check(
        development.snapshot().ble.secure,
        "radio restart must retain the in-memory bond");
    development.ble_reboot();
    check(
        !development.snapshot().ble.bonded,
        "development cold restart must clear the bond");

    Simulator production(false);
    check(production.ready(), "production BLE simulator must be ready");
    check(production.ble_connect(), "production BLE connect failed");
    check(
        production.ble_confirm_pairing(123'456),
        "production BLE pairing failed");
    check(production.ble_provision(1), "production BLE settings failed");
    production.ble_reboot();
    check(
        production.snapshot().ble.bonded,
        "production cold restart must retain the bond");
    check(
        production.snapshot().ble.active_revision == 1,
        "production cold restart must retain settings metadata");

    check(
        !production.ble_fuzz(0, 1),
        "zero BLE fuzz cases must be rejected");
    check(
        !production.ble_fuzz(
            chatesp::simulator::kMaximumBleFuzzCases + 1, 1),
        "an excessive BLE fuzz run must be rejected");
    check(
        production.ble_fuzz(20'000, 0x1234abcdU),
        "bounded BLE protocol fuzz run must complete");
    check(
        production.snapshot().ble.fuzz_cases == 20'000,
        "BLE fuzz run must report its bounded case count");

    Simulator voice(true);
    check(voice.ready(), "voice BLE simulator must be ready");
    check(voice.ble_connect(), "voice BLE connection must start");
    check(voice.ble_confirm_pairing(123'456), "voice BLE pairing failed");
    check(voice.action_button(true), "recording press must start");
    check(voice.advance(600), "recording hold must advance");
    check(voice.action_button(false), "recording release must submit");
    check(
        voice.snapshot().ble.state == chatesp::simulator::BleState::off,
        "voice request must stop BLE");
    check(voice.fail_interaction(), "voice request failure must run");
    check(
        voice.snapshot().ble.state ==
            chatesp::simulator::BleState::advertising,
        "voice request completion must restart BLE advertising");
}

}  // namespace

int main() {
    test_short_press_sleeps_and_clears_private_text();
    test_voice_flow_returns_to_clock();
    test_mode_and_pairing_orientation();
    test_touch_controls_are_bounded();
    test_status_is_private_and_svg_is_explicit();
    test_invalid_text_and_wake_paths();
    test_large_advance_is_bounded_and_keeps_intermediate_time();
    test_answer_limit_and_clock_network_window();
    test_ble_pairing_security_and_retry_flow();
    test_ble_faults_recover_without_partial_state();
    test_ble_bond_restart_and_sanitized_fuzz();
    if (failures != 0) {
        std::cerr << failures << " simulator test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All simulator tests passed\n";
    return EXIT_SUCCESS;
}
