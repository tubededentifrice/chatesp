#include "chatesp/ip_location.hpp"

#include <cstdint>
#include <limits>

#include "json.hpp"

namespace chatesp {
namespace agent {
namespace {

bool read_signed_seconds(detail::JsonReader &reader, std::int32_t &seconds) {
    const bool negative = reader.peek() == '-';
    if (negative && !reader.consume('-')) {
        return false;
    }
    std::uint32_t magnitude = 0;
    if (!reader.read_u32(magnitude) ||
        magnitude > static_cast<std::uint32_t>(
                        std::numeric_limits<std::int32_t>::max())) {
        return false;
    }
    seconds = negative ? -static_cast<std::int32_t>(magnitude)
                       : static_cast<std::int32_t>(magnitude);
    return true;
}

bool parse_timezone(
    detail::JsonReader &reader, std::int16_t &offset_minutes,
    bool &has_offset) {
    if (!reader.consume('{')) {
        return false;
    }
    if (reader.peek() == '}') {
        return reader.consume('}');
    }
    while (true) {
        FixedText<48> key;
        if (!reader.read_string(key) || !reader.consume(':')) {
            return false;
        }
        if (key.equals("offset")) {
            std::int32_t offset_seconds = 0;
            if (!read_signed_seconds(reader, offset_seconds) ||
                offset_seconds < -14 * 3'600 ||
                offset_seconds > 14 * 3'600 ||
                offset_seconds % 60 != 0) {
                return false;
            }
            offset_minutes =
                static_cast<std::int16_t>(offset_seconds / 60);
            has_offset = true;
        } else if (!reader.skip_value()) {
            return false;
        }
        if (reader.consume('}')) {
            return true;
        }
        if (!reader.consume(',')) {
            return false;
        }
    }
}

bool append_location_part(
    FixedText<96> &output, const FixedText<64> &part) {
    if (part.empty()) {
        return true;
    }
    if (!output.empty() && !output.append(", ")) {
        return false;
    }
    return output.append(part.data(), part.size());
}

bool valid_location_part(const FixedText<64> &part) {
    for (std::size_t index = 0; index < part.size(); ++index) {
        const auto value = static_cast<unsigned char>(part.data()[index]);
        if (value < 0x20 || value == 0x7f) {
            return false;
        }
    }
    return true;
}

bool valid_country_code(const FixedText<64> &country_code) {
    return country_code.size() == 2 && country_code.data()[0] >= 'A' &&
        country_code.data()[0] <= 'Z' && country_code.data()[1] >= 'A' &&
        country_code.data()[1] <= 'Z';
}

}  // namespace

Error parse_ip_location_response(
    const char *json, std::size_t size, IpLocationContext &context) {
    context.clear();
    if (json == nullptr || size == 0 || size > 512) {
        return Error::malformed_response;
    }

    detail::JsonReader reader(json, size);
    FixedText<64> city;
    FixedText<64> region;
    FixedText<64> country_code;
    bool success = false;
    bool has_success = false;
    bool has_offset = false;
    if (!reader.consume('{')) {
        return Error::malformed_response;
    }
    if (reader.peek() != '}') {
        while (true) {
            FixedText<48> key;
            if (!reader.read_string(key) || !reader.consume(':')) {
                return Error::malformed_response;
            }
            if (key.equals("success")) {
                if (reader.consume_literal("true")) {
                    success = true;
                } else if (reader.consume_literal("false")) {
                    success = false;
                } else {
                    return Error::malformed_response;
                }
                has_success = true;
            } else if (key.equals("city")) {
                if (!reader.read_string(city)) {
                    return Error::malformed_response;
                }
            } else if (key.equals("region")) {
                if (!reader.read_string(region)) {
                    return Error::malformed_response;
                }
            } else if (key.equals("country_code")) {
                if (!reader.read_string(country_code)) {
                    return Error::malformed_response;
                }
            } else if (key.equals("timezone")) {
                if (!parse_timezone(
                        reader, context.utc_offset_minutes, has_offset)) {
                    return Error::malformed_response;
                }
            } else if (!reader.skip_value()) {
                return Error::malformed_response;
            }
            if (reader.consume('}')) {
                break;
            }
            if (!reader.consume(',')) {
                return Error::malformed_response;
            }
        }
    } else if (!reader.consume('}')) {
        return Error::malformed_response;
    }
    if (!reader.finish() || !has_success || !success || !has_offset ||
        !valid_location_part(city) || !valid_location_part(region) ||
        !valid_country_code(country_code)) {
        context.clear();
        return Error::malformed_response;
    }

    if (!append_location_part(context.approximate_location, city) ||
        (!region.equals(city.c_str()) &&
         !append_location_part(context.approximate_location, region)) ||
        !append_location_part(context.approximate_location, country_code) ||
        context.approximate_location.empty()) {
        context.clear();
        return Error::limit_exceeded;
    }
    return Error::none;
}

}  // namespace agent
}  // namespace chatesp
