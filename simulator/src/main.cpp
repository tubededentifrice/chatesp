#include "chatesp/simulator/simulator.hpp"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using chatesp::ClockTime;
using chatesp::simulator::BleFault;
using chatesp::simulator::Simulator;
using chatesp::simulator::WifiState;

constexpr std::size_t kMaximumCommandBytes = 4'096;
constexpr std::size_t kMaximumScenarioBytes = 64 * 1'024;

enum class CommandResult {
    accepted,
    rejected,
    quit,
};

std::string_view trim_left(std::string_view value) {
    const std::size_t first = value.find_first_not_of(" \t");
    return first == std::string_view::npos ? std::string_view{}
                                           : value.substr(first);
}

template <typename Integer>
bool parse_integer(std::string_view text, Integer &value) {
    if (text.empty()) {
        return false;
    }
    const char *first = text.data();
    const char *last = first + text.size();
    const auto result = std::from_chars(first, last, value);
    return result.ec == std::errc{} && result.ptr == last;
}

bool parse_clock(std::string_view text, ClockTime &time) {
    if (text.size() != 8 || text[2] != ':' || text[5] != ':') {
        return false;
    }
    for (const std::size_t index : {0U, 1U, 3U, 4U, 6U, 7U}) {
        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
    }
    time.hour = static_cast<std::uint8_t>(
        (text[0] - '0') * 10 + text[1] - '0');
    time.minute = static_cast<std::uint8_t>(
        (text[3] - '0') * 10 + text[4] - '0');
    time.second = static_cast<std::uint8_t>(
        (text[6] - '0') * 10 + text[7] - '0');
    return time.valid();
}

bool matches_expectation(
    const Simulator &simulator,
    std::string_view field,
    std::string_view expected) {
    const auto value = simulator.snapshot();
    const auto boolean = [expected](bool actual) {
        return expected == (actual ? "true" : "false");
    };
    const auto number = [expected](auto actual) {
        return expected == std::to_string(actual);
    };
    if (field == "state") {
        return expected == chatesp::state_name(value.interaction);
    }
    if (field == "now_ms") {
        return number(value.now_ms);
    }
    if (field == "mode") {
        return expected == (value.mode == chatesp::AppMode::chat
            ? "chat" : "clock");
    }
    if (field == "orientation") {
        return expected == (value.orientation == chatesp::DisplayOrientation::chat
            ? "chat" : "clock");
    }
    if (field == "screen_on") {
        return boolean(value.screen_on);
    }
    if (field == "wifi") {
        return expected == chatesp::simulator::wifi_state_name(value.wifi);
    }
    if (field == "brightness") {
        return number(static_cast<unsigned>(value.brightness_percent));
    }
    if (field == "volume") {
        return number(static_cast<unsigned>(value.volume_percent));
    }
    if (field == "battery") {
        return number(static_cast<unsigned>(value.battery_percent));
    }
    if (field == "pairing_visible") {
        return boolean(value.pairing_code_visible);
    }
    if (field == "controls_open") {
        return boolean(value.controls_open);
    }
    if (field == "transcript_bytes") {
        return number(value.transcript_bytes);
    }
    if (field == "answer_bytes") {
        return number(value.answer_bytes);
    }
    if (field == "event_fuzz_cases") {
        return number(value.event_fuzz_cases);
    }
    if (field == "ble.state") {
        return expected == chatesp::simulator::ble_state_name(value.ble.state);
    }
    if (field == "ble.outcome") {
        return expected == chatesp::simulator::ble_outcome_name(value.ble.outcome);
    }
    if (field == "ble.active_revision") {
        return number(value.ble.active_revision);
    }
    if (field == "ble.fuzz_cases") {
        return number(value.ble.fuzz_cases);
    }
    return false;
}

void print_help(std::ostream &output) {
    output
        << "version\nstatus\nreset\nready\nadvance MILLISECONDS\n"
           "action down|up\nmode DURATION_MILLISECONDS\n"
           "transcript TEXT\ntool\nanswer TEXT\nfinish\nfail\n"
           "pairing show SIX_DIGIT_CODE\npairing hide\n"
           "ble connect\nble pair SIX_DIGIT_CODE\nble reject\n"
           "ble disconnect\nble radio on|off|restart\nble reboot\n"
           "ble provision REVISION [none|disconnect-after-control|"
           "disconnect-after-data|drop-ack|corrupt-data|storage-failure]\n"
           "ble fuzz CASES SEED\n"
           "fuzz CASES SEED\nexpect FIELD VALUE\n"
           "touch down X Y\ntouch up X Y\n"
           "controls brightness PERCENT\ncontrols volume PERCENT\n"
           "clock HH:MM:SS|unavailable\n"
           "wifi setup|off|connecting|online|failed\n"
           "battery PERCENT|unavailable\n"
           "power connected|battery\nrender PATH\nquit\n";
}

CommandResult process_command(Simulator &simulator, std::string_view line) {
    line = trim_left(line);
    if (line == "version") {
        return CommandResult::accepted;
    }
    if (line == "status") {
        return CommandResult::accepted;
    }
    if (line == "reset") {
        simulator.reset();
        return CommandResult::accepted;
    }
    if (line == "ready") {
        return simulator.ready() ? CommandResult::accepted
                                 : CommandResult::rejected;
    }
    if (line == "tool") {
        return simulator.start_tool() ? CommandResult::accepted
                                      : CommandResult::rejected;
    }
    if (line == "finish") {
        return simulator.finish_interaction() ? CommandResult::accepted
                                              : CommandResult::rejected;
    }
    if (line == "fail") {
        return simulator.fail_interaction() ? CommandResult::accepted
                                            : CommandResult::rejected;
    }
    if (line == "pairing hide") {
        simulator.hide_pairing_code();
        return CommandResult::accepted;
    }
    if (line == "ble connect") {
        return simulator.ble_connect() ? CommandResult::accepted
                                       : CommandResult::rejected;
    }
    if (line == "ble reject") {
        return simulator.ble_reject_pairing() ? CommandResult::accepted
                                              : CommandResult::rejected;
    }
    if (line == "ble disconnect") {
        return simulator.ble_disconnect() ? CommandResult::accepted
                                          : CommandResult::rejected;
    }
    if (line == "ble radio on") {
        simulator.ble_start_radio();
        return CommandResult::accepted;
    }
    if (line == "ble radio off") {
        simulator.ble_stop_radio();
        return CommandResult::accepted;
    }
    if (line == "ble radio restart") {
        simulator.ble_restart_radio();
        return CommandResult::accepted;
    }
    if (line == "ble reboot") {
        simulator.ble_reboot();
        return CommandResult::accepted;
    }
    if (line == "quit") {
        return CommandResult::quit;
    }
    if (line == "help") {
        print_help(std::cerr);
        return CommandResult::accepted;
    }

    constexpr std::string_view advance_prefix = "advance ";
    if (line.substr(0, advance_prefix.size()) == advance_prefix) {
        std::uint32_t milliseconds = 0;
        if (!parse_integer(line.substr(advance_prefix.size()), milliseconds)) {
            return CommandResult::rejected;
        }
        return simulator.advance(milliseconds) ? CommandResult::accepted
                                               : CommandResult::rejected;
    }

    constexpr std::string_view mode_prefix = "mode ";
    if (line.substr(0, mode_prefix.size()) == mode_prefix) {
        std::uint32_t duration = 0;
        return parse_integer(line.substr(mode_prefix.size()), duration) &&
                simulator.mode_button(duration)
            ? CommandResult::accepted
            : CommandResult::rejected;
    }

    if (line == "action down") {
        return simulator.action_button(true) ? CommandResult::accepted
                                             : CommandResult::rejected;
    }
    if (line == "action up") {
        return simulator.action_button(false) ? CommandResult::accepted
                                              : CommandResult::rejected;
    }

    constexpr std::string_view transcript_prefix = "transcript ";
    if (line.substr(0, transcript_prefix.size()) == transcript_prefix) {
        return simulator.set_transcript(line.substr(transcript_prefix.size()))
            ? CommandResult::accepted
            : CommandResult::rejected;
    }
    constexpr std::string_view answer_prefix = "answer ";
    if (line.substr(0, answer_prefix.size()) == answer_prefix) {
        return simulator.set_answer(line.substr(answer_prefix.size()))
            ? CommandResult::accepted
            : CommandResult::rejected;
    }

    constexpr std::string_view pairing_prefix = "pairing show ";
    if (line.substr(0, pairing_prefix.size()) == pairing_prefix) {
        const std::string_view code_text = line.substr(pairing_prefix.size());
        std::uint32_t code = 0;
        return code_text.size() == 6 && parse_integer(code_text, code) &&
                simulator.show_pairing_code(code)
            ? CommandResult::accepted
            : CommandResult::rejected;
    }

    constexpr std::string_view ble_pair_prefix = "ble pair ";
    if (line.substr(0, ble_pair_prefix.size()) == ble_pair_prefix) {
        const std::string_view code_text = line.substr(ble_pair_prefix.size());
        std::uint32_t code = 0;
        return code_text.size() == 6 && parse_integer(code_text, code) &&
                simulator.ble_confirm_pairing(code)
            ? CommandResult::accepted
            : CommandResult::rejected;
    }

    constexpr std::string_view ble_provision_prefix = "ble provision ";
    if (line.substr(0, ble_provision_prefix.size()) ==
        ble_provision_prefix) {
        std::istringstream input{
            std::string(line.substr(ble_provision_prefix.size()))};
        std::uint32_t revision = 0;
        std::string fault_text;
        std::string extra;
        if (!(input >> revision)) {
            return CommandResult::rejected;
        }
        if (!(input >> fault_text)) {
            fault_text = "none";
        }
        if (input >> extra) {
            return CommandResult::rejected;
        }
        BleFault fault = BleFault::none;
        return chatesp::simulator::parse_ble_fault(fault_text, fault) &&
                simulator.ble_provision(revision, fault)
            ? CommandResult::accepted
            : CommandResult::rejected;
    }

    constexpr std::string_view ble_fuzz_prefix = "ble fuzz ";
    if (line.substr(0, ble_fuzz_prefix.size()) == ble_fuzz_prefix) {
        std::istringstream input{
            std::string(line.substr(ble_fuzz_prefix.size()))};
        std::size_t cases = 0;
        std::uint32_t seed = 0;
        std::string extra;
        return (input >> cases >> seed) && !(input >> extra) &&
                simulator.ble_fuzz(cases, seed)
            ? CommandResult::accepted
            : CommandResult::rejected;
    }

    constexpr std::string_view fuzz_prefix = "fuzz ";
    if (line.substr(0, fuzz_prefix.size()) == fuzz_prefix) {
        std::istringstream input{
            std::string(line.substr(fuzz_prefix.size()))};
        std::size_t cases = 0;
        std::uint32_t seed = 0;
        std::string extra;
        return (input >> cases >> seed) && !(input >> extra) &&
                simulator.event_fuzz(cases, seed)
            ? CommandResult::accepted
            : CommandResult::rejected;
    }

    constexpr std::string_view expect_prefix = "expect ";
    if (line.substr(0, expect_prefix.size()) == expect_prefix) {
        std::istringstream input{
            std::string(line.substr(expect_prefix.size()))};
        std::string field;
        std::string expected;
        std::string extra;
        return (input >> field >> expected) && !(input >> extra) &&
                matches_expectation(simulator, field, expected)
            ? CommandResult::accepted
            : CommandResult::rejected;
    }

    if (line.substr(0, 6) == "touch ") {
        std::istringstream input{std::string(line.substr(6))};
        std::string action;
        int x = 0;
        int y = 0;
        std::string extra;
        if (!(input >> action >> x >> y) || (input >> extra) ||
            x < std::numeric_limits<std::int16_t>::min() ||
            x > std::numeric_limits<std::int16_t>::max() ||
            y < std::numeric_limits<std::int16_t>::min() ||
            y > std::numeric_limits<std::int16_t>::max()) {
            return CommandResult::rejected;
        }
        const auto bounded_x = static_cast<std::int16_t>(x);
        const auto bounded_y = static_cast<std::int16_t>(y);
        if (action == "down") {
            return simulator.touch_down(bounded_x, bounded_y)
                ? CommandResult::accepted
                : CommandResult::rejected;
        }
        if (action == "up") {
            return simulator.touch_up(bounded_x, bounded_y)
                ? CommandResult::accepted
                : CommandResult::rejected;
        }
        return CommandResult::rejected;
    }

    if (line.substr(0, 9) == "controls ") {
        std::istringstream input{std::string(line.substr(9))};
        std::string control;
        std::uint32_t value = 0;
        std::string extra;
        if (!(input >> control >> value) || (input >> extra)) {
            return CommandResult::rejected;
        }
        if (control == "brightness") {
            return simulator.set_brightness(value)
                ? CommandResult::accepted
                : CommandResult::rejected;
        }
        if (control == "volume") {
            return simulator.set_volume(value) ? CommandResult::accepted
                                               : CommandResult::rejected;
        }
        return CommandResult::rejected;
    }

    constexpr std::string_view clock_prefix = "clock ";
    if (line.substr(0, clock_prefix.size()) == clock_prefix) {
        const std::string_view value = line.substr(clock_prefix.size());
        if (value == "unavailable") {
            return simulator.set_clock_time(false) ? CommandResult::accepted
                                                   : CommandResult::rejected;
        }
        ClockTime time;
        return parse_clock(value, time) && simulator.set_clock_time(true, time)
            ? CommandResult::accepted
            : CommandResult::rejected;
    }

    constexpr std::string_view wifi_prefix = "wifi ";
    if (line.substr(0, wifi_prefix.size()) == wifi_prefix) {
        const std::string_view value = line.substr(wifi_prefix.size());
        if (value == "setup") {
            simulator.set_wifi(WifiState::setup);
        } else if (value == "off") {
            simulator.set_wifi(WifiState::off);
        } else if (value == "connecting") {
            simulator.set_wifi(WifiState::connecting);
        } else if (value == "online") {
            simulator.set_wifi(WifiState::online);
        } else if (value == "failed") {
            simulator.set_wifi(WifiState::failed);
        } else {
            return CommandResult::rejected;
        }
        return CommandResult::accepted;
    }

    constexpr std::string_view battery_prefix = "battery ";
    if (line.substr(0, battery_prefix.size()) == battery_prefix) {
        const std::string_view value = line.substr(battery_prefix.size());
        if (value == "unavailable") {
            return simulator.set_battery(false) ? CommandResult::accepted
                                                : CommandResult::rejected;
        }
        std::uint32_t percent = 0;
        return parse_integer(value, percent) &&
                simulator.set_battery(true, percent)
            ? CommandResult::accepted
            : CommandResult::rejected;
    }

    constexpr std::string_view power_prefix = "power ";
    if (line.substr(0, power_prefix.size()) == power_prefix) {
        const std::string_view value = line.substr(power_prefix.size());
        if (value == "connected") {
            simulator.set_external_power(true);
        } else if (value == "battery") {
            simulator.set_external_power(false);
        } else {
            return CommandResult::rejected;
        }
        return CommandResult::accepted;
    }

    constexpr std::string_view render_prefix = "render ";
    if (line.substr(0, render_prefix.size()) == render_prefix) {
        const std::string_view path = trim_left(line.substr(render_prefix.size()));
        return !path.empty() && path.size() <=
                chatesp::simulator::kMaximumArtifactPathBytes &&
                simulator.render_svg(std::string(path))
            ? CommandResult::accepted
            : CommandResult::rejected;
    }

    return CommandResult::rejected;
}

bool run_stream(Simulator &simulator, std::istream &input) {
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::string_view trimmed = trim_left(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        if (line.size() > kMaximumCommandBytes) {
            std::cout << simulator.status_json(false) << '\n';
            return false;
        }
        const CommandResult result = process_command(simulator, line);
        if (result == CommandResult::quit) {
            std::cout << simulator.status_json(true) << '\n';
            return true;
        }
        const bool accepted = result == CommandResult::accepted;
        std::cout << simulator.status_json(accepted) << '\n';
        if (!accepted) {
            return false;
        }
    }
    return input.eof();
}

struct Options {
    std::optional<std::string> scenario;
    std::optional<std::string> render;
    bool development_mode = false;
};

bool parse_options(int argc, char **argv, Options &options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--development") {
            options.development_mode = true;
        } else if (argument == "--scenario" && index + 1 < argc) {
            options.scenario = argv[++index];
        } else if (argument == "--render" && index + 1 < argc) {
            options.render = argv[++index];
        } else if (argument == "--help") {
            print_help(std::cout);
            return false;
        } else {
            std::cerr << "Invalid simulator option\n";
            return false;
        }
    }
    const auto path_is_valid = [](const std::optional<std::string> &path) {
        return !path.has_value() ||
            (!path->empty() &&
             path->size() <= chatesp::simulator::kMaximumArtifactPathBytes);
    };
    return path_is_valid(options.scenario) && path_is_valid(options.render);
}

}  // namespace

int main(int argc, char **argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        return argc == 2 && std::string_view(argv[1]) == "--help" ? 0 : 2;
    }

    Simulator simulator(options.development_mode);
    bool success = false;
    if (options.scenario.has_value()) {
        std::ifstream input(*options.scenario, std::ios::binary | std::ios::ate);
        if (!input || input.tellg() < 0 ||
            static_cast<std::uint64_t>(input.tellg()) > kMaximumScenarioBytes) {
            std::cerr << "Scenario file is not available or is too large\n";
            return 2;
        }
        input.seekg(0);
        success = run_stream(simulator, input);
    } else {
        success = run_stream(simulator, std::cin);
    }
    if (!success) {
        return 2;
    }
    if (options.render.has_value() && !simulator.render_svg(*options.render)) {
        std::cerr << "Display artifact could not be written\n";
        return 2;
    }
    return 0;
}
