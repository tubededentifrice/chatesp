#include "chatesp/simulator/svg_renderer.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace chatesp::simulator {
namespace {

std::string xml_escape(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (const char value : input) {
        switch (value) {
            case '&':
                output += "&amp;";
                break;
            case '<':
                output += "&lt;";
                break;
            case '>':
                output += "&gt;";
                break;
            case '"':
                output += "&quot;";
                break;
            case '\'':
                output += "&apos;";
                break;
            default:
                output += value;
                break;
        }
    }
    return output;
}

void render_footer(std::ostringstream &output, const Snapshot &snapshot) {
    output << "<text x=\"18\" y=\"426\" class=\"footer\">WI-FI "
           << wifi_state_name(snapshot.wifi) << "</text>";
    output << "<text x=\"350\" y=\"426\" text-anchor=\"end\" "
              "class=\"footer\">BAT ";
    if (snapshot.battery_available) {
        output << static_cast<unsigned>(snapshot.battery_percent) << '%';
    } else {
        output << "--";
    }
    output << "</text>";
}

void render_controls(std::ostringstream &output, const Snapshot &snapshot) {
    if (!snapshot.controls_open) {
        output << "<rect x=\"164\" y=\"8\" width=\"40\" height=\"4\" "
                  "rx=\"2\" fill=\"#606060\"/>";
        return;
    }
    output << "<g id=\"quick-controls\">"
              "<rect x=\"8\" y=\"8\" width=\"352\" height=\"232\" "
              "rx=\"18\" fill=\"#050505\" stroke=\"#303030\"/>"
              "<text x=\"24\" y=\"48\" class=\"label\">BRIGHTNESS</text>"
              "<rect x=\"24\" y=\"70\" width=\"320\" height=\"6\" "
              "rx=\"3\" fill=\"#303030\"/>";
    const int brightness_width =
        320 * static_cast<int>(snapshot.brightness_percent) / 100;
    output << "<rect x=\"24\" y=\"70\" width=\"" << brightness_width
           << "\" height=\"6\" rx=\"3\" fill=\"white\"/>"
              "<text x=\"24\" y=\"132\" class=\"label\">VOLUME</text>"
              "<rect x=\"24\" y=\"154\" width=\"320\" height=\"6\" "
              "rx=\"3\" fill=\"#303030\"/>";
    const int volume_width =
        320 * static_cast<int>(snapshot.volume_percent) / 100;
    output << "<rect x=\"24\" y=\"154\" width=\"" << volume_width
           << "\" height=\"6\" rx=\"3\" fill=\"white\"/>"
              "<text x=\"24\" y=\"210\" class=\"footer\">SWIPE UP TO CLOSE</text>"
              "</g>";
}

void render_recording_bars(
    std::ostringstream &output, const Snapshot &snapshot) {
    if (snapshot.interaction != InteractionState::recording) {
        return;
    }
    output << "<g id=\"spectrum\">";
    for (int index = 0; index < 18; ++index) {
        const int height = 24 + static_cast<int>(
            (snapshot.now_ms / 17U + static_cast<std::uint32_t>(index * 29)) %
            94U);
        const int x = 20 + index * 19;
        const int y = 298 - height;
        output << "<rect x=\"" << x << "\" y=\"" << y
               << "\" width=\"11\" height=\"" << height
               << "\" rx=\"5\" fill=\"white\"/>";
    }
    output << "</g>";
}

void render_pairing(std::ostringstream &output, const Snapshot &snapshot) {
    if (!snapshot.pairing_code_visible) {
        return;
    }
    output << "<rect x=\"0\" y=\"0\" width=\"368\" height=\"448\" "
              "fill=\"black\"/>"
              "<text x=\"24\" y=\"72\" class=\"status\">PAIRING CODE</text>"
              "<text x=\"184\" y=\"232\" text-anchor=\"middle\" "
              "class=\"pairing\">"
           << std::setw(6) << std::setfill('0') << snapshot.pairing_code
           << "</text>"
              "<text x=\"184\" y=\"284\" text-anchor=\"middle\" "
              "class=\"hint\">CONFIRM ON IPHONE</text>";
}

void render_chat(std::ostringstream &output, const DisplayView &view) {
    const Snapshot &snapshot = view.snapshot;
    output << "<text x=\"18\" y=\"40\" class=\"brand\">CHAT ESP</text>"
              "<text x=\"18\" y=\"78\" class=\"status\">"
           << state_name(snapshot.interaction) << "</text>";

    if (!view.transcript.empty()) {
        output << "<text x=\"18\" y=\"116\" class=\"label\">YOU</text>"
                  "<foreignObject x=\"18\" y=\"128\" width=\"332\" "
                  "height=\"94\"><div xmlns=\"http://www.w3.org/1999/xhtml\" "
                  "class=\"body\">"
               << xml_escape(view.transcript) << "</div></foreignObject>";
    }
    if (!view.answer.empty()) {
        output << "<text x=\"18\" y=\"246\" class=\"label\">ANSWER</text>"
                  "<foreignObject x=\"18\" y=\"258\" width=\"332\" "
                  "height=\"138\"><div xmlns=\"http://www.w3.org/1999/xhtml\" "
                  "class=\"body\">"
               << xml_escape(view.answer) << "</div></foreignObject>";
    }
    render_recording_bars(output, snapshot);
    render_footer(output, snapshot);
    render_controls(output, snapshot);
    render_pairing(output, snapshot);
}

void render_clock(std::ostringstream &output, const Snapshot &snapshot) {
    const ClockTime time = snapshot.clock_time;
    const ClockPathSpan span = snapshot.clock_time_available
        ? clock_path_span(
              time.minute,
              static_cast<std::uint32_t>(time.second) * 1'000U +
                  time.millisecond,
              60'000)
        : ClockPathSpan{};
    if (span.count != 0) {
        output << "<path d=\"M224 10 H390 Q438 10 438 58 V310 "
                  "Q438 358 390 358 H58 Q10 358 10 310 V58 Q10 10 58 10 Z\" "
                  "pathLength=\"60000\" fill=\"none\" stroke=\"white\" "
                  "stroke-width=\"1\" "
                  "stroke-dasharray=\""
               << static_cast<unsigned>(span.count) << ' '
               << static_cast<unsigned>(60'000U - span.count)
               << "\" stroke-dashoffset=\"-"
               << static_cast<unsigned>(span.first) << "\"/>";
    }

    output << "<text x=\"224\" y=\"236\" text-anchor=\"middle\" "
              "class=\"clock\">";
    if (snapshot.clock_time_available && time.valid()) {
        output << std::setw(2) << std::setfill('0')
               << static_cast<unsigned>(time.hour) << ':' << std::setw(2)
               << static_cast<unsigned>(time.minute);
    } else {
        output << "--:--";
    }
    output << "</text>";
}

}  // namespace

std::string render_svg_text(const DisplayView &view) {
    const bool clock_orientation =
        view.snapshot.orientation == DisplayOrientation::clock;
    const int width = clock_orientation ? 448 : 368;
    const int height = clock_orientation ? 368 : 448;
    std::ostringstream output;
    output << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
              "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\""
           << width << "\" height=\"" << height << "\" viewBox=\"0 0 "
           << width << ' ' << height << "\">"
              "<style>"
              "text,.body{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;fill:white;color:white}"
              ".brand{font-size:16px;font-weight:700;letter-spacing:3px}"
              ".status{font-size:26px;font-weight:700}"
              ".label{font-size:13px;letter-spacing:2px;fill:#a0a0a0}"
              ".hint,.footer{font-size:11px;letter-spacing:1px;fill:#909090}"
              ".body{font-size:18px;line-height:1.35;overflow:hidden;overflow-wrap:anywhere}"
              ".pairing{font-size:48px;font-weight:700;letter-spacing:4px}"
              ".clock{font-size:124px;font-weight:700;letter-spacing:-10px}"
              "</style><rect width=\"100%\" height=\"100%\" fill=\"black\"/>";
    if (view.snapshot.screen_on) {
        if (clock_orientation) {
            render_clock(output, view.snapshot);
        } else {
            render_chat(output, view);
        }
    }
    output << "</svg>";
    return output.str();
}

}  // namespace chatesp::simulator
