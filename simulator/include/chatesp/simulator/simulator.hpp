#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "chatesp/app_mode.hpp"
#include "chatesp/interaction_state.hpp"
#include "chatesp/quick_controls.hpp"
#include "chatesp/simulator/ble_simulator.hpp"

namespace chatesp::simulator {

constexpr std::size_t kMaximumTranscriptBytes = 2'048;
constexpr std::size_t kMaximumAnswerBytes = 1'280;
constexpr std::size_t kMaximumPrivateTextBytes = kMaximumTranscriptBytes;
constexpr std::size_t kMaximumArtifactPathBytes = 1'024;
constexpr std::uint32_t kCommandProtocolVersion = 1;
constexpr std::uint32_t kMaximumAdvanceMs = 10 * 60'000;
constexpr std::size_t kMaximumEventFuzzCases = 100'000;

enum class WifiState : std::uint8_t {
    setup,
    off,
    connecting,
    online,
    failed,
};

struct Snapshot {
    std::uint32_t now_ms = 0;
    InteractionState interaction = InteractionState::booting;
    AppMode mode = AppMode::chat;
    DisplayOrientation orientation = DisplayOrientation::chat;
    WifiState wifi = WifiState::setup;
    ClockTime clock_time{};
    std::uint8_t brightness_percent = 65;
    std::uint8_t volume_percent = 70;
    std::uint8_t battery_percent = 0;
    std::size_t transcript_bytes = 0;
    std::size_t answer_bytes = 0;
    std::uint32_t pairing_code = 0;
    bool screen_on = true;
    bool clock_time_available = false;
    bool battery_available = false;
    bool external_power_connected = false;
    bool pairing_code_visible = false;
    bool controls_open = false;
    std::size_t event_fuzz_cases = 0;
    // Compatibility field. Clock entry is never automatic.
    bool return_to_clock_pending = false;
    bool clock_network_shutdown_pending = false;
    BleSnapshot ble{};
};

struct DisplayView {
    Snapshot snapshot;
    std::string_view transcript;
    std::string_view answer;
};

class Simulator {
public:
    explicit Simulator(bool development_mode = false);

    void reset();
    bool ready();
    bool advance(std::uint32_t milliseconds);
    bool action_button(bool pressed);
    bool mode_button(std::uint32_t duration_ms);
    bool set_transcript(std::string_view text);
    bool start_tool();
    bool set_answer(std::string_view text);
    bool finish_interaction();
    bool fail_interaction();
    bool show_pairing_code(std::uint32_t code);
    void hide_pairing_code();
    bool touch_down(std::int16_t x, std::int16_t y);
    bool touch_up(std::int16_t x, std::int16_t y);
    bool set_brightness(std::uint32_t percent);
    bool set_volume(std::uint32_t percent);
    bool set_clock_time(bool available, ClockTime time = {});
    void set_wifi(WifiState state);
    bool set_battery(bool available, std::uint32_t percent = 0);
    void set_external_power(bool connected);
    bool ble_connect();
    bool ble_confirm_pairing(std::uint32_t passkey);
    bool ble_reject_pairing();
    bool ble_disconnect();
    void ble_start_radio();
    void ble_stop_radio();
    void ble_restart_radio();
    void ble_reboot();
    bool ble_provision(
        std::uint32_t revision,
        BleFault fault = BleFault::none);
    bool ble_fuzz(std::size_t cases, std::uint32_t seed);
    bool event_fuzz(std::size_t cases, std::uint32_t seed);

    [[nodiscard]] Snapshot snapshot() const;
    [[nodiscard]] DisplayView display_view() const;
    [[nodiscard]] std::string status_json(bool ok = true) const;
    [[nodiscard]] bool render_svg(const std::string &path) const;

private:
    bool set_private_text(
        std::array<char, kMaximumPrivateTextBytes + 1> &destination,
        std::size_t &destination_size,
        std::string_view text,
        std::size_t maximum_size);
    void clear_private_text();
    void enter_clock();
    void process_time();
    void refresh_controls_allowed();
    [[nodiscard]] bool invariants_hold() const;

    InteractionStateMachine interaction_;
    BleSimulator ble_;
    ShortPressGesture mode_button_;
    QuickControlsGesture controls_;
    AppMode mode_ = AppMode::chat;
    WifiState wifi_ = WifiState::setup;
    ClockTime clock_time_{};
    std::array<char, kMaximumPrivateTextBytes + 1> transcript_{};
    std::array<char, kMaximumPrivateTextBytes + 1> answer_{};
    std::uint32_t now_ms_ = 0;
    std::uint32_t clock_entered_at_ms_ = 0;
    std::uint32_t clock_unpowered_since_ms_ = 0;
    std::uint32_t pairing_code_ = 0;
    std::size_t transcript_size_ = 0;
    std::size_t answer_size_ = 0;
    std::size_t event_fuzz_cases_ = 0;
    std::uint8_t brightness_percent_ = 65;
    std::uint8_t volume_percent_ = 70;
    std::uint8_t battery_percent_ = 0;
    bool screen_on_ = true;
    bool clock_time_available_ = false;
    bool battery_available_ = false;
    bool external_power_connected_ = false;
    bool pairing_code_visible_ = false;
    bool clock_network_shutdown_pending_ = false;
};

[[nodiscard]] const char *wifi_state_name(WifiState state);

}  // namespace chatesp::simulator
