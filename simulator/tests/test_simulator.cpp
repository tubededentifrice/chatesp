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
    if (failures != 0) {
        std::cerr << failures << " simulator test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All simulator tests passed\n";
    return EXIT_SUCCESS;
}
