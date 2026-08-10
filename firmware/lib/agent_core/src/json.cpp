#include "json.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace chatesp {
namespace agent {
namespace detail {

void JsonReader::skip_space() {
    while (position_ < size_ &&
           std::isspace(static_cast<unsigned char>(data_[position_])) != 0) {
        ++position_;
    }
}

char JsonReader::peek() {
    skip_space();
    return position_ < size_ ? data_[position_] : '\0';
}

bool JsonReader::consume(char expected) {
    skip_space();
    if (position_ == size_ || data_[position_] != expected) {
        return false;
    }
    ++position_;
    return true;
}

bool JsonReader::consume_literal(const char *literal) {
    skip_space();
    if (literal == nullptr) {
        return false;
    }
    const std::size_t length = std::strlen(literal);
    if (length > size_ - position_ ||
        std::memcmp(data_ + position_, literal, length) != 0) {
        return false;
    }
    position_ += length;
    return true;
}

bool JsonReader::read_hex4(std::uint32_t &value) {
    if (size_ - position_ < 4) {
        return false;
    }
    value = 0;
    for (int index = 0; index < 4; ++index) {
        const char digit = data_[position_++];
        value <<= 4;
        if (digit >= '0' && digit <= '9') {
            value |= static_cast<std::uint32_t>(digit - '0');
        } else if (digit >= 'a' && digit <= 'f') {
            value |= static_cast<std::uint32_t>(digit - 'a' + 10);
        } else if (digit >= 'A' && digit <= 'F') {
            value |= static_cast<std::uint32_t>(digit - 'A' + 10);
        } else {
            return false;
        }
    }
    return true;
}

bool JsonReader::read_u32(std::uint32_t &value) {
    skip_space();
    if (position_ == size_ || data_[position_] < '0' ||
        data_[position_] > '9') {
        return false;
    }
    std::uint64_t result = 0;
    while (position_ < size_ && data_[position_] >= '0' &&
           data_[position_] <= '9') {
        result = result * 10 + static_cast<unsigned>(data_[position_] - '0');
        if (result > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        ++position_;
    }
    value = static_cast<std::uint32_t>(result);
    return true;
}

bool JsonReader::read_double(double &value) {
    skip_space();
    const std::size_t start = position_;
    std::size_t cursor = start;
    if (cursor < size_ && data_[cursor] == '-') {
        ++cursor;
    }
    if (cursor == size_) {
        return false;
    }
    if (data_[cursor] == '0') {
        ++cursor;
    } else {
        if (data_[cursor] < '1' || data_[cursor] > '9') {
            return false;
        }
        while (cursor < size_ && data_[cursor] >= '0' &&
               data_[cursor] <= '9') {
            ++cursor;
        }
    }
    if (cursor < size_ && data_[cursor] == '.') {
        ++cursor;
        const std::size_t decimal = cursor;
        while (cursor < size_ && data_[cursor] >= '0' &&
               data_[cursor] <= '9') {
            ++cursor;
        }
        if (cursor == decimal) {
            return false;
        }
    }
    if (cursor < size_ &&
        (data_[cursor] == 'e' || data_[cursor] == 'E')) {
        ++cursor;
        if (cursor < size_ &&
            (data_[cursor] == '+' || data_[cursor] == '-')) {
            ++cursor;
        }
        const std::size_t exponent = cursor;
        while (cursor < size_ && data_[cursor] >= '0' &&
               data_[cursor] <= '9') {
            ++cursor;
        }
        if (cursor == exponent) {
            return false;
        }
    }

    constexpr std::size_t max_number_bytes = 63;
    const std::size_t length = cursor - start;
    if (length == 0 || length > max_number_bytes) {
        return false;
    }
    char number[max_number_bytes + 1]{};
    std::memcpy(number, data_ + start, length);
    char *end = nullptr;
    const double parsed = std::strtod(number, &end);
    if (end != number + length || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    position_ = cursor;
    return true;
}

bool JsonReader::skip_value(std::uint8_t depth) {
    if (depth > 12) {
        return false;
    }
    const char current = peek();
    if (current == '"') {
        // Scan without storing so that skipped provider fields stay bounded.
        if (!consume('"')) return false;
        bool escaped = false;
        while (position_ < size_) {
            const unsigned char value =
                static_cast<unsigned char>(data_[position_++]);
            if (!escaped && value == '"') return true;
            if (!escaped && value < 0x20) return false;
            if (!escaped && value == '\\') {
                escaped = true;
            } else if (escaped) {
                if (value == 'u') {
                    std::uint32_t ignored_hex = 0;
                    if (!read_hex4(ignored_hex)) return false;
                } else if (std::strchr("\"\\/bfnrt", value) == nullptr) {
                    return false;
                }
                escaped = false;
            }
        }
        return false;
    }
    if (current == '{') {
        if (!consume('{')) return false;
        if (peek() == '}') return consume('}');
        while (true) {
            FixedText<128> key;
            if (!read_string(key) || !consume(':') ||
                !skip_value(static_cast<std::uint8_t>(depth + 1))) {
                return false;
            }
            if (consume('}')) return true;
            if (!consume(',')) return false;
        }
    }
    if (current == '[') {
        if (!consume('[')) return false;
        if (peek() == ']') return consume(']');
        while (true) {
            if (!skip_value(static_cast<std::uint8_t>(depth + 1))) return false;
            if (consume(']')) return true;
            if (!consume(',')) return false;
        }
    }
    if (current == 't') return consume_literal("true");
    if (current == 'f') return consume_literal("false");
    if (current == 'n') return consume_literal("null");

    skip_space();
    const std::size_t start = position_;
    if (position_ < size_ && data_[position_] == '-') ++position_;
    if (position_ == size_) return false;
    if (data_[position_] == '0') {
        ++position_;
    } else {
        if (data_[position_] < '1' || data_[position_] > '9') return false;
        while (position_ < size_ && data_[position_] >= '0' &&
               data_[position_] <= '9') {
            ++position_;
        }
    }
    if (position_ < size_ && data_[position_] == '.') {
        ++position_;
        const std::size_t decimal = position_;
        while (position_ < size_ && data_[position_] >= '0' &&
               data_[position_] <= '9') {
            ++position_;
        }
        if (position_ == decimal) return false;
    }
    if (position_ < size_ &&
        (data_[position_] == 'e' || data_[position_] == 'E')) {
        ++position_;
        if (position_ < size_ &&
            (data_[position_] == '+' || data_[position_] == '-')) {
            ++position_;
        }
        const std::size_t exponent = position_;
        while (position_ < size_ && data_[position_] >= '0' &&
               data_[position_] <= '9') {
            ++position_;
        }
        if (position_ == exponent) return false;
    }
    return position_ > start;
}

bool JsonReader::finish() {
    skip_space();
    return position_ == size_;
}

bool valid_json_value(const char *data, std::size_t size) {
    if (data == nullptr || size == 0) {
        return false;
    }
    JsonReader reader(data, size);
    return reader.skip_value() && reader.finish();
}

}  // namespace detail
}  // namespace agent
}  // namespace chatesp
