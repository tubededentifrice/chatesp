#include "chatesp/brave_protocol.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>

#include "json.hpp"

namespace chatesp {
namespace agent {
namespace {

bool valid_option(const char *value, std::size_t max_size) {
    if (value == nullptr) return false;
    const std::size_t size = std::strlen(value);
    if (size == 0 || size > max_size) return false;
    for (std::size_t index = 0; index < size; ++index) {
        const unsigned char current = static_cast<unsigned char>(value[index]);
        if (std::isalnum(current) == 0 && current != '-') return false;
    }
    return true;
}

bool append_url_encoded(
    SearchRequestTarget &target, const char *text, std::size_t size) {
    constexpr char hex[] = "0123456789ABCDEF";
    for (std::size_t index = 0; index < size; ++index) {
        const unsigned char value = static_cast<unsigned char>(text[index]);
        if ((value >= 'A' && value <= 'Z') ||
            (value >= 'a' && value <= 'z') ||
            (value >= '0' && value <= '9') || value == '-' || value == '_' ||
            value == '.' || value == '~') {
            if (!target.push_back(static_cast<char>(value))) return false;
        } else {
            const char encoded[] = {
                '%', hex[(value >> 4) & 0x0F], hex[value & 0x0F]};
            if (!target.append(encoded, sizeof(encoded))) return false;
        }
    }
    return true;
}

Error build_target(
    const char *base, const char *query, std::size_t size,
    const BraveSearchOptions &options, SearchRequestTarget &target) {
    target.clear();
    if (query == nullptr || size == 0 ||
        size > Limits::max_search_query_bytes ||
        !valid_option(options.country, 3) ||
        !valid_option(options.search_language, 8)) {
        return Error::invalid_argument;
    }
    if (!target.append(base) || !append_url_encoded(target, query, size) ||
        !target.append("&country=") || !target.append(options.country) ||
        !target.append("&search_lang=") ||
        !target.append(options.search_language)) {
        target.clear();
        return Error::limit_exceeded;
    }
    return Error::none;
}

bool parse_web_result(detail::JsonReader &reader, WebResult &result) {
    if (!reader.consume('{')) return false;
    if (reader.peek() == '}') return reader.consume('}');
    while (true) {
        FixedText<48> key;
        if (!reader.read_string(key) || !reader.consume(':')) return false;
        if (key.equals("title")) {
            if (!reader.read_string(result.title)) return false;
        } else if (key.equals("url")) {
            if (!reader.read_string(result.url)) return false;
        } else if (key.equals("description")) {
            if (!reader.read_string(result.snippet)) return false;
        } else if (!reader.skip_value()) {
            return false;
        }
        if (reader.consume('}')) return true;
        if (!reader.consume(',')) return false;
    }
}

bool parse_web_array(detail::JsonReader &reader, WebResults &results) {
    if (!reader.consume('[')) return false;
    if (reader.peek() == ']') return reader.consume(']');
    while (true) {
        if (results.size < Limits::max_web_results) {
            WebResult candidate;
            if (!parse_web_result(reader, candidate)) return false;
            if (!candidate.title.empty() && !candidate.url.empty()) {
                results.items[results.size++] = candidate;
            }
        } else if (!reader.skip_value()) {
            return false;
        }
        if (reader.consume(']')) return true;
        if (!reader.consume(',')) return false;
    }
}

bool parse_web_container(detail::JsonReader &reader, WebResults &results) {
    if (!reader.consume('{')) return false;
    if (reader.peek() == '}') return reader.consume('}');
    while (true) {
        FixedText<48> key;
        if (!reader.read_string(key) || !reader.consume(':')) return false;
        if (key.equals("results")) {
            if (!parse_web_array(reader, results)) return false;
        } else if (!reader.skip_value()) {
            return false;
        }
        if (reader.consume('}')) return true;
        if (!reader.consume(',')) return false;
    }
}

bool parse_thumbnail(detail::JsonReader &reader, ImageResult &result) {
    if (!reader.consume('{')) return false;
    if (reader.peek() == '}') return reader.consume('}');
    while (true) {
        FixedText<48> key;
        if (!reader.read_string(key) || !reader.consume(':')) return false;
        if (key.equals("src")) {
            if (!reader.read_string(result.thumbnail_url)) return false;
        } else if (!reader.skip_value()) {
            return false;
        }
        if (reader.consume('}')) return true;
        if (!reader.consume(',')) return false;
    }
}

bool parse_properties(detail::JsonReader &reader, ImageResult &result) {
    if (!reader.consume('{')) return false;
    if (reader.peek() == '}') return reader.consume('}');
    while (true) {
        FixedText<48> key;
        if (!reader.read_string(key) || !reader.consume(':')) return false;
        if (key.equals("url")) {
            if (!reader.read_string(result.image_url)) return false;
        } else if (key.equals("width")) {
            if (!reader.read_u32(result.width)) return false;
        } else if (key.equals("height")) {
            if (!reader.read_u32(result.height)) return false;
        } else if (!reader.skip_value()) {
            return false;
        }
        if (reader.consume('}')) return true;
        if (!reader.consume(',')) return false;
    }
}

bool parse_image_result(detail::JsonReader &reader, ImageResult &result) {
    if (!reader.consume('{')) return false;
    if (reader.peek() == '}') return reader.consume('}');
    while (true) {
        FixedText<48> key;
        if (!reader.read_string(key) || !reader.consume(':')) return false;
        if (key.equals("title")) {
            if (!reader.read_string(result.title)) return false;
        } else if (key.equals("url")) {
            if (!reader.read_string(result.page_url)) return false;
        } else if (key.equals("thumbnail")) {
            if (!parse_thumbnail(reader, result)) return false;
        } else if (key.equals("properties")) {
            if (!parse_properties(reader, result)) return false;
        } else if (!reader.skip_value()) {
            return false;
        }
        if (reader.consume('}')) return true;
        if (!reader.consume(',')) return false;
    }
}

bool parse_image_array(detail::JsonReader &reader, ImageResults &results) {
    if (!reader.consume('[')) return false;
    if (reader.peek() == ']') return reader.consume(']');
    while (true) {
        if (results.size < Limits::max_image_results) {
            ImageResult candidate;
            if (!parse_image_result(reader, candidate)) return false;
            if (!candidate.title.empty() && !candidate.thumbnail_url.empty()) {
                char identifier[Limits::max_image_id_bytes + 1]{};
                const int written = std::snprintf(
                    identifier, sizeof(identifier), "img%u",
                    static_cast<unsigned>(results.size));
                if (written <= 0 ||
                    !candidate.id.assign(
                        identifier, static_cast<std::size_t>(written))) {
                    return false;
                }
                results.items[results.size++] = candidate;
            }
        } else if (!reader.skip_value()) {
            return false;
        }
        if (reader.consume(']')) return true;
        if (!reader.consume(',')) return false;
    }
}

}  // namespace

Error build_brave_web_search_target(
    const char *query, std::size_t size, const BraveSearchOptions &options,
    SearchRequestTarget &target) {
    return build_target(
        "/res/v1/web/search?q=", query, size, options, target) == Error::none &&
                   target.append(
                       "&count=5&result_filter=web&safesearch=strict&"
                       "text_decorations=false")
               ? Error::none
               : (target.empty() ? Error::invalid_argument
                                 : Error::limit_exceeded);
}

Error build_brave_image_search_target(
    const char *query, std::size_t size, const BraveSearchOptions &options,
    SearchRequestTarget &target) {
    return build_target(
        "/res/v1/images/search?q=", query, size, options, target) ==
                       Error::none &&
                   target.append("&count=6&safesearch=strict")
               ? Error::none
               : (target.empty() ? Error::invalid_argument
                                 : Error::limit_exceeded);
}

Error parse_brave_web_response(
    const char *json, std::size_t size, WebResults &results) {
    results.clear();
    if (json == nullptr || size == 0 || size > Limits::max_web_response_bytes) {
        return Error::limit_exceeded;
    }
    detail::JsonReader reader(json, size);
    if (!reader.consume('{')) return Error::malformed_response;
    bool found = false;
    while (reader.peek() != '}') {
        FixedText<48> key;
        if (!reader.read_string(key) || !reader.consume(':')) {
            return Error::malformed_response;
        }
        if (key.equals("web")) {
            if (found || !parse_web_container(reader, results)) {
                return Error::malformed_response;
            }
            found = true;
        } else if (!reader.skip_value()) {
            return Error::malformed_response;
        }
        if (reader.consume('}')) break;
        if (!reader.consume(',')) return Error::malformed_response;
    }
    return reader.finish() && found ? Error::none : Error::malformed_response;
}

Error parse_brave_image_response(
    const char *json, std::size_t size, ImageResults &results) {
    results.clear();
    if (json == nullptr || size == 0 ||
        size > Limits::max_image_search_response_bytes) {
        return Error::limit_exceeded;
    }
    detail::JsonReader reader(json, size);
    if (!reader.consume('{')) return Error::malformed_response;
    bool found = false;
    while (reader.peek() != '}') {
        FixedText<48> key;
        if (!reader.read_string(key) || !reader.consume(':')) {
            return Error::malformed_response;
        }
        if (key.equals("results")) {
            if (found || !parse_image_array(reader, results)) {
                return Error::malformed_response;
            }
            found = true;
        } else if (!reader.skip_value()) {
            return Error::malformed_response;
        }
        if (reader.consume('}')) break;
        if (!reader.consume(',')) return Error::malformed_response;
    }
    return reader.finish() && found ? Error::none : Error::malformed_response;
}

}  // namespace agent
}  // namespace chatesp
