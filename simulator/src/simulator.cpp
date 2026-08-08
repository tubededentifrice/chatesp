#include "chatesp/simulator/simulator.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "chatesp/simulator/svg_renderer.hpp"

namespace chatesp::simulator {
namespace {

const char *mode_name(AppMode mode) {
    return mode == AppMode::chat ? "chat" : "clock";
}

const char *orientation_name(DisplayOrientation orientation) {
    return orientation == DisplayOrientation::chat ? "chat" : "clock";
}

bool valid_private_text(std::string_view text) {
    const auto continuation = [](unsigned char value) {
        return value >= 0x80U && value <= 0xbfU;
    };
    for (std::size_t index = 0; index < text.size();) {
        const auto first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7fU) {
            if (first < 0x20U && first != '\t' && first != '\n' &&
                first != '\r') {
                return false;
            }
            ++index;
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU) {
            if (index + 1 >= text.size() || !continuation(
                    static_cast<unsigned char>(text[index + 1]))) {
                return false;
            }
            index += 2;
            continue;
        }
        if (first >= 0xe0U && first <= 0xefU) {
            if (index + 2 >= text.size()) {
                return false;
            }
            const auto second =
                static_cast<unsigned char>(text[index + 1]);
            const auto third =
                static_cast<unsigned char>(text[index + 2]);
            const bool second_valid = first == 0xe0U
                ? second >= 0xa0U && second <= 0xbfU
                : (first == 0xedU
                       ? second >= 0x80U && second <= 0x9fU
                       : continuation(second));
            if (!second_valid || !continuation(third)) {
                return false;
            }
            index += 3;
            continue;
        }
        if (first >= 0xf0U && first <= 0xf4U) {
            if (index + 3 >= text.size()) {
                return false;
            }
            const auto second =
                static_cast<unsigned char>(text[index + 1]);
            const bool second_valid = first == 0xf0U
                ? second >= 0x90U && second <= 0xbfU
                : (first == 0xf4U
                       ? second >= 0x80U && second <= 0x8fU
                       : continuation(second));
            if (!second_valid ||
                !continuation(
                    static_cast<unsigned char>(text[index + 2])) ||
                !continuation(
                    static_cast<unsigned char>(text[index + 3]))) {
                return false;
            }
            index += 4;
            continue;
        }
        return false;
    }
    return true;
}

}  // namespace

Simulator::Simulator(bool development_mode)
    : development_mode_(development_mode),
      interaction_(interaction_config_for_mode(development_mode)),
      ble_(development_mode) {
    refresh_controls_allowed();
}

void Simulator::reset() {
    interaction_ = InteractionStateMachine(
        interaction_config_for_mode(development_mode_));
    ble_.factory_reset();
    mode_button_.cancel();
    controls_ = QuickControlsGesture();
    mode_ = AppMode::chat;
    wifi_ = WifiState::setup;
    clock_time_ = {};
    now_ms_ = 0;
    clock_entered_at_ms_ = 0;
    pairing_code_ = 0;
    brightness_percent_ = 65;
    volume_percent_ = 70;
    battery_percent_ = 0;
    screen_on_ = true;
    clock_time_available_ = false;
    battery_available_ = false;
    pairing_code_visible_ = false;
    return_to_clock_pending_ = false;
    clock_network_shutdown_pending_ = false;
    clear_private_text();
    refresh_controls_allowed();
}

bool Simulator::ready() {
    if (interaction_.state() != InteractionState::booting) {
        return false;
    }
    interaction_.ready(now_ms_);
    ble_.start_radio();
    refresh_controls_allowed();
    return true;
}

bool Simulator::advance(std::uint32_t milliseconds) {
    if (milliseconds > kMaximumAdvanceMs) {
        return false;
    }
    constexpr std::uint32_t kMaximumTickStepMs = 10;
    std::uint32_t remaining = milliseconds;
    do {
        const std::uint32_t step =
            std::min(remaining, kMaximumTickStepMs);
        now_ms_ += step;
        process_time();
        remaining -= step;
    } while (remaining != 0);
    return true;
}

void Simulator::process_time() {
    if (clock_return_due(
            mode_, return_to_clock_pending_,
            interaction_.state() == InteractionState::idle,
            interaction_.inactivity_ms(now_ms_))) {
        enter_clock();
    } else if (mode_ == AppMode::chat && screen_on_) {
        interaction_.tick(now_ms_);
    }

    if (interaction_.state() == InteractionState::sleep_pending) {
        screen_on_ = false;
        ble_.stop_radio();
        return_to_clock_pending_ = false;
        pairing_code_visible_ = false;
        controls_.set_allowed(false);
        clear_private_text();
    } else if (controls_.automatic_close_due(now_ms_)) {
        controls_.set_open(false, now_ms_);
    }
    if (mode_ == AppMode::clock && clock_network_shutdown_due(
            clock_network_shutdown_pending_, clock_time_available_,
            now_ms_ - clock_entered_at_ms_, 15'000)) {
        wifi_ = WifiState::off;
        clock_network_shutdown_pending_ = false;
    }
    refresh_controls_allowed();
}

bool Simulator::action_button(bool pressed) {
    if (pressed) {
        clear_private_text();
        pairing_code_visible_ = false;
        controls_.set_open(false, now_ms_);
        return_to_clock_pending_ = false;
        clock_network_shutdown_pending_ = false;
        if (!screen_on_ ||
            interaction_.state() == InteractionState::sleep_pending) {
            screen_on_ = true;
            mode_ = AppMode::chat;
            interaction_ = InteractionStateMachine(
                interaction_config_for_mode(development_mode_));
            interaction_.ready(now_ms_);
            interaction_.wake_button_down(now_ms_);
            ble_.start_radio();
            refresh_controls_allowed();
            return true;
        }
        if (mode_ == AppMode::clock) {
            mode_ = AppMode::chat;
            interaction_.ready(now_ms_);
        }
        interaction_.button_down(now_ms_);
    } else {
        if (!interaction_.button_is_down()) {
            return false;
        }
        interaction_.button_up(now_ms_);
        if (interaction_.state() == InteractionState::transcribing) {
            ble_.stop_radio();
        }
        process_time();
    }
    refresh_controls_allowed();
    return true;
}

bool Simulator::mode_button(std::uint32_t duration_ms) {
    if (duration_ms > kMaximumAdvanceMs) {
        return false;
    }
    if (!screen_on_ ||
        interaction_.state() == InteractionState::sleep_pending) {
        return advance(duration_ms);
    }
    mode_button_.press(now_ms_);
    if (!advance(duration_ms) ||
        interaction_.state() == InteractionState::sleep_pending) {
        mode_button_.cancel();
        return true;
    }
    const bool accepted = mode_button_.release(now_ms_);
    if (!accepted) {
        return true;
    }
    if (mode_ == AppMode::chat) {
        enter_clock();
    } else {
        mode_ = AppMode::chat;
        interaction_.ready(now_ms_);
        return_to_clock_pending_ = false;
        clock_network_shutdown_pending_ = false;
    }
    refresh_controls_allowed();
    return true;
}

bool Simulator::set_transcript(std::string_view text) {
    if (interaction_.state() != InteractionState::transcribing ||
        !set_private_text(
            transcript_, transcript_size_, text,
            kMaximumTranscriptBytes)) {
        return false;
    }
    interaction_.transcription_ready(now_ms_);
    refresh_controls_allowed();
    return true;
}

bool Simulator::start_tool() {
    if (interaction_.state() != InteractionState::thinking) {
        return false;
    }
    interaction_.tool_started(now_ms_);
    refresh_controls_allowed();
    return true;
}

bool Simulator::set_answer(std::string_view text) {
    const InteractionState state = interaction_.state();
    if ((state != InteractionState::thinking &&
        state != InteractionState::tool_work) ||
        !set_private_text(
            answer_, answer_size_, text, kMaximumAnswerBytes)) {
        return false;
    }
    interaction_.speech_started(now_ms_);
    refresh_controls_allowed();
    return true;
}

bool Simulator::finish_interaction() {
    const InteractionState prior = interaction_.state();
    if (prior != InteractionState::transcribing &&
        prior != InteractionState::thinking &&
        prior != InteractionState::tool_work &&
        prior != InteractionState::speaking) {
        return false;
    }
    interaction_.interaction_finished(now_ms_);
    ble_.start_radio();
    return_to_clock_pending_ = true;
    refresh_controls_allowed();
    return true;
}

bool Simulator::fail_interaction() {
    if (interaction_.state() == InteractionState::sleep_pending) {
        return false;
    }
    interaction_.fail(now_ms_);
    ble_.start_radio();
    return_to_clock_pending_ = false;
    clear_private_text();
    refresh_controls_allowed();
    return true;
}

bool Simulator::show_pairing_code(std::uint32_t code) {
    if (code > 999'999U) {
        return false;
    }
    pairing_code_ = code;
    pairing_code_visible_ = true;
    screen_on_ = true;
    controls_.set_open(false, now_ms_);
    refresh_controls_allowed();
    return true;
}

void Simulator::hide_pairing_code() {
    pairing_code_visible_ = false;
    pairing_code_ = 0;
    refresh_controls_allowed();
}

bool Simulator::touch_down(
    std::int16_t x, std::int16_t y) {
    refresh_controls_allowed();
    if (!controls_.allowed()) {
        return false;
    }
    controls_.press(x, y, now_ms_);
    return controls_.pressed();
}

bool Simulator::touch_up(std::int16_t x, std::int16_t y) {
    if (!controls_.pressed()) {
        return false;
    }
    const QuickControlsAction action = controls_.release(x, y, now_ms_);
    if (action == QuickControlsAction::open) {
        controls_.set_open(true, now_ms_);
    } else if (action == QuickControlsAction::close) {
        controls_.set_open(false, now_ms_);
    }
    if (interaction_.state() == InteractionState::idle) {
        interaction_.note_idle_activity(now_ms_);
    }
    return true;
}

bool Simulator::set_brightness(std::uint32_t percent) {
    if (!controls_.open() || percent < 5 || percent > 100) {
        return false;
    }
    brightness_percent_ = QuickControlsGesture::snap_percent(
        static_cast<std::int32_t>(percent), 5);
    controls_.note_activity(now_ms_);
    interaction_.note_idle_activity(now_ms_);
    return true;
}

bool Simulator::set_volume(std::uint32_t percent) {
    if (!controls_.open() || percent > 100) {
        return false;
    }
    volume_percent_ = QuickControlsGesture::snap_percent(
        static_cast<std::int32_t>(percent), 0);
    controls_.note_activity(now_ms_);
    interaction_.note_idle_activity(now_ms_);
    return true;
}

bool Simulator::set_clock_time(bool available, ClockTime time) {
    if (available && !time.valid()) {
        return false;
    }
    clock_time_available_ = available;
    clock_time_ = available ? time : ClockTime{};
    process_time();
    return true;
}

void Simulator::set_wifi(WifiState state) { wifi_ = state; }

bool Simulator::set_battery(bool available, std::uint32_t percent) {
    if (available && percent > 100) {
        return false;
    }
    battery_available_ = available;
    battery_percent_ = available ? static_cast<std::uint8_t>(percent) : 0;
    return true;
}

bool Simulator::ble_connect() {
    const bool accepted = ble_.connect();
    const BleSnapshot state = ble_.snapshot();
    if (accepted && state.passkey_visible) {
        (void)show_pairing_code(state.passkey);
    }
    return accepted;
}

bool Simulator::ble_confirm_pairing(std::uint32_t passkey) {
    const bool accepted = ble_.confirm_pairing(passkey);
    if (accepted) {
        hide_pairing_code();
    }
    return accepted;
}

bool Simulator::ble_reject_pairing() {
    const bool accepted = ble_.reject_pairing();
    if (accepted) {
        hide_pairing_code();
    }
    return accepted;
}

bool Simulator::ble_disconnect() {
    const bool accepted = ble_.disconnect();
    if (accepted) {
        hide_pairing_code();
    }
    return accepted;
}

void Simulator::ble_start_radio() { ble_.start_radio(); }

void Simulator::ble_stop_radio() {
    ble_.stop_radio();
    hide_pairing_code();
}

void Simulator::ble_restart_radio() {
    ble_.restart_radio();
    hide_pairing_code();
}

void Simulator::ble_reboot() {
    ble_.reboot();
    hide_pairing_code();
}

bool Simulator::ble_provision(std::uint32_t revision, BleFault fault) {
    const bool accepted = ble_.provision(revision, fault);
    const BleSnapshot state = ble_.snapshot();
    if (!state.passkey_visible) {
        hide_pairing_code();
    }
    return accepted;
}

bool Simulator::ble_fuzz(std::size_t cases, std::uint32_t seed) {
    return ble_.fuzz(cases, seed);
}

Snapshot Simulator::snapshot() const {
    Snapshot value;
    value.now_ms = now_ms_;
    value.interaction = interaction_.state();
    value.mode = mode_;
    value.orientation = display_orientation_for(
        mode_, pairing_code_visible_);
    value.wifi = wifi_;
    value.clock_time = clock_time_;
    value.brightness_percent = brightness_percent_;
    value.volume_percent = volume_percent_;
    value.battery_percent = battery_percent_;
    value.transcript_bytes = transcript_size_;
    value.answer_bytes = answer_size_;
    value.pairing_code = pairing_code_;
    value.screen_on = screen_on_;
    value.clock_time_available = clock_time_available_;
    value.battery_available = battery_available_;
    value.pairing_code_visible = pairing_code_visible_;
    value.controls_open = controls_.open();
    value.return_to_clock_pending = return_to_clock_pending_;
    value.clock_network_shutdown_pending =
        clock_network_shutdown_pending_;
    value.ble = ble_.snapshot();
    return value;
}

DisplayView Simulator::display_view() const {
    return DisplayView{
        snapshot(),
        std::string_view(transcript_.data(), transcript_size_),
        std::string_view(answer_.data(), answer_size_),
    };
}

std::string Simulator::status_json(bool ok) const {
    const Snapshot value = snapshot();
    std::ostringstream output;
    output << "{\"protocol\":" << kCommandProtocolVersion
           << ",\"ok\":" << (ok ? "true" : "false")
           << ",\"now_ms\":" << value.now_ms
           << ",\"state\":\"" << state_name(value.interaction) << '"'
           << ",\"mode\":\"" << mode_name(value.mode) << '"'
           << ",\"orientation\":\""
           << orientation_name(value.orientation) << '"'
           << ",\"screen_on\":"
           << (value.screen_on ? "true" : "false")
           << ",\"wifi\":\"" << wifi_state_name(value.wifi) << '"'
           << ",\"brightness\":"
           << static_cast<unsigned>(value.brightness_percent)
           << ",\"volume\":"
           << static_cast<unsigned>(value.volume_percent)
           << ",\"battery_available\":"
           << (value.battery_available ? "true" : "false")
           << ",\"battery\":"
           << static_cast<unsigned>(value.battery_percent)
           << ",\"pairing_visible\":"
           << (value.pairing_code_visible ? "true" : "false")
           << ",\"controls_open\":"
           << (value.controls_open ? "true" : "false")
           << ",\"transcript_bytes\":" << value.transcript_bytes
           << ",\"answer_bytes\":" << value.answer_bytes
           << ",\"return_to_clock_pending\":"
           << (value.return_to_clock_pending ? "true" : "false")
           << ",\"clock_network_shutdown_pending\":"
           << (value.clock_network_shutdown_pending ? "true" : "false")
           << ",\"ble\":{\"state\":\""
           << ble_state_name(value.ble.state) << '"'
           << ",\"secure\":" << (value.ble.secure ? "true" : "false")
           << ",\"bonded\":" << (value.ble.bonded ? "true" : "false")
           << ",\"outcome\":\""
           << ble_outcome_name(value.ble.outcome) << '"'
           << ",\"active_revision\":" << value.ble.active_revision
           << ",\"attempts\":" << value.ble.attempts
           << ",\"storage_writes\":" << value.ble.storage_writes
           << ",\"fuzz_cases\":" << value.ble.fuzz_cases << '}'
           << '}';
    return output.str();
}

bool Simulator::render_svg(const std::string &path) const {
    if (path.empty() || path.size() > kMaximumArtifactPathBytes) {
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    const std::string svg = render_svg_text(display_view());
    output.write(svg.data(), static_cast<std::streamsize>(svg.size()));
    return output.good();
}

bool Simulator::set_private_text(
    std::array<char, kMaximumPrivateTextBytes + 1> &destination,
    std::size_t &destination_size,
    std::string_view text,
    std::size_t maximum_size) {
    if (text.empty() || text.size() > maximum_size ||
        !valid_private_text(text)) {
        return false;
    }
    std::fill(destination.begin(), destination.end(), '\0');
    std::memcpy(destination.data(), text.data(), text.size());
    destination_size = text.size();
    return true;
}

void Simulator::clear_private_text() {
    std::fill(transcript_.begin(), transcript_.end(), '\0');
    std::fill(answer_.begin(), answer_.end(), '\0');
    transcript_size_ = 0;
    answer_size_ = 0;
}

void Simulator::enter_clock() {
    mode_ = AppMode::clock;
    clock_entered_at_ms_ = now_ms_;
    clock_network_shutdown_pending_ = !clock_time_available_;
    if (clock_time_available_) {
        wifi_ = WifiState::off;
    }
    interaction_.ready(now_ms_);
    return_to_clock_pending_ = false;
    controls_.set_open(false, now_ms_);
    clear_private_text();
}

void Simulator::refresh_controls_allowed() {
    const bool allowed = screen_on_ && !pairing_code_visible_ &&
        interaction_.state() == InteractionState::idle;
    if (controls_.allowed() != allowed) {
        controls_.set_allowed(allowed);
    }
}

const char *wifi_state_name(WifiState state) {
    switch (state) {
        case WifiState::setup:
            return "setup";
        case WifiState::off:
            return "off";
        case WifiState::connecting:
            return "connecting";
        case WifiState::online:
            return "online";
        case WifiState::failed:
            return "failed";
    }
    return "unknown";
}

}  // namespace chatesp::simulator
