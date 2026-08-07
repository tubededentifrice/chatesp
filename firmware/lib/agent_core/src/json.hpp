#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "chatesp/fixed_text.hpp"

namespace chatesp {
namespace agent {
namespace detail {

class JsonReader {
public:
    JsonReader(const char *data, std::size_t size) : data_(data), size_(size) {}

    bool consume(char expected);
    bool consume_literal(const char *literal);
    bool skip_value(std::uint8_t depth = 0);
    bool read_u32(std::uint32_t &value);
    bool finish();
    [[nodiscard]] char peek();

    template <std::size_t Capacity>
    bool read_string(FixedText<Capacity> &output) {
        output.clear();
        if (!consume('"')) {
            return false;
        }
        while (position_ < size_) {
            const unsigned char current =
                static_cast<unsigned char>(data_[position_++]);
            if (current == '"') {
                return true;
            }
            if (current < 0x20) {
                return false;
            }
            if (current != '\\') {
                if (!output.push_back(static_cast<char>(current))) {
                    return false;
                }
                continue;
            }
            if (position_ == size_) {
                return false;
            }
            const char escaped = data_[position_++];
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    if (!output.push_back(escaped)) {
                        return false;
                    }
                    break;
                case 'b':
                    if (!output.push_back('\b')) {
                        return false;
                    }
                    break;
                case 'f':
                    if (!output.push_back('\f')) {
                        return false;
                    }
                    break;
                case 'n':
                    if (!output.push_back('\n')) {
                        return false;
                    }
                    break;
                case 'r':
                    if (!output.push_back('\r')) {
                        return false;
                    }
                    break;
                case 't':
                    if (!output.push_back('\t')) {
                        return false;
                    }
                    break;
                case 'u': {
                    std::uint32_t code_point = 0;
                    if (!read_hex4(code_point)) {
                        return false;
                    }
                    if (code_point >= 0xD800 && code_point <= 0xDBFF) {
                        if (position_ + 2 > size_ || data_[position_] != '\\' ||
                            data_[position_ + 1] != 'u') {
                            return false;
                        }
                        position_ += 2;
                        std::uint32_t low = 0;
                        if (!read_hex4(low) || low < 0xDC00 || low > 0xDFFF) {
                            return false;
                        }
                        code_point = 0x10000 +
                                     ((code_point - 0xD800) << 10) +
                                     (low - 0xDC00);
                    } else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
                        return false;
                    }
                    if (!append_utf8(code_point, output)) {
                        return false;
                    }
                    break;
                }
                default:
                    return false;
            }
        }
        return false;
    }

private:
    void skip_space();
    bool read_hex4(std::uint32_t &value);

    template <std::size_t Capacity>
    static bool append_utf8(
        std::uint32_t code_point, FixedText<Capacity> &output) {
        if (code_point <= 0x7F) {
            return output.push_back(static_cast<char>(code_point));
        }
        if (code_point <= 0x7FF) {
            return output.push_back(static_cast<char>(0xC0 | (code_point >> 6))) &&
                   output.push_back(
                       static_cast<char>(0x80 | (code_point & 0x3F)));
        }
        if (code_point <= 0xFFFF) {
            return output.push_back(static_cast<char>(0xE0 | (code_point >> 12))) &&
                   output.push_back(static_cast<char>(
                       0x80 | ((code_point >> 6) & 0x3F))) &&
                   output.push_back(
                       static_cast<char>(0x80 | (code_point & 0x3F)));
        }
        if (code_point <= 0x10FFFF) {
            return output.push_back(static_cast<char>(0xF0 | (code_point >> 18))) &&
                   output.push_back(static_cast<char>(
                       0x80 | ((code_point >> 12) & 0x3F))) &&
                   output.push_back(static_cast<char>(
                       0x80 | ((code_point >> 6) & 0x3F))) &&
                   output.push_back(
                       static_cast<char>(0x80 | (code_point & 0x3F)));
        }
        return false;
    }

    const char *data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t position_ = 0;
};

template <std::size_t Capacity>
bool append_json_string(
    FixedText<Capacity> &output, const char *text, std::size_t size) {
    if (text == nullptr || !output.push_back('"')) {
        return false;
    }
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t index = 0; index < size; ++index) {
        const unsigned char value = static_cast<unsigned char>(text[index]);
        switch (value) {
            case '"':
                if (!output.append("\\\"")) return false;
                break;
            case '\\':
                if (!output.append("\\\\")) return false;
                break;
            case '\b':
                if (!output.append("\\b")) return false;
                break;
            case '\f':
                if (!output.append("\\f")) return false;
                break;
            case '\n':
                if (!output.append("\\n")) return false;
                break;
            case '\r':
                if (!output.append("\\r")) return false;
                break;
            case '\t':
                if (!output.append("\\t")) return false;
                break;
            default:
                if (value < 0x20) {
                    char escaped[] = {'\\', 'u', '0', '0', hex[value >> 4],
                                      hex[value & 0x0F]};
                    if (!output.append(escaped, sizeof(escaped))) return false;
                } else if (!output.push_back(static_cast<char>(value))) {
                    return false;
                }
        }
    }
    return output.push_back('"');
}

template <std::size_t Capacity>
bool append_json_string(FixedText<Capacity> &output, const char *text) {
    return text != nullptr && append_json_string(output, text, std::strlen(text));
}

bool valid_json_value(const char *data, std::size_t size);

}  // namespace detail
}  // namespace agent
}  // namespace chatesp
