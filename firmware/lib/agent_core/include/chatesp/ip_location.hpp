#pragma once

#include <cstddef>
#include <cstdint>

#include "chatesp/agent_types.hpp"

namespace chatesp {
namespace agent {

struct IpLocationContext {
    FixedText<96> approximate_location;
    std::int16_t utc_offset_minutes = 0;

    void clear() {
        approximate_location.clear();
        utc_offset_minutes = 0;
    }
};

Error parse_ip_location_response(
    const char *json, std::size_t size, IpLocationContext &context);

}  // namespace agent
}  // namespace chatesp
