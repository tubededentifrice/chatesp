#pragma once

#include <string>

#include "chatesp/simulator/simulator.hpp"

namespace chatesp::simulator {

[[nodiscard]] std::string render_svg_text(const DisplayView &view);

}  // namespace chatesp::simulator
