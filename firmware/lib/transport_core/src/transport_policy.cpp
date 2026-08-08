#include "chatesp/transport_policy.hpp"

#include <cctype>
#include <cstring>

namespace chatesp {
namespace transport {
namespace {

bool ascii_equal_fold(char left, char right) {
    return std::tolower(static_cast<unsigned char>(left)) ==
           std::tolower(static_cast<unsigned char>(right));
}

bool text_equal_fold(const char *left, std::size_t size, const char *right) {
    if (std::strlen(right) != size) {
        return false;
    }
    for (std::size_t index = 0; index < size; ++index) {
        if (!ascii_equal_fold(left[index], right[index])) {
            return false;
        }
    }
    return true;
}

bool contains_control(const char *value) {
    if (value == nullptr) {
        return true;
    }
    for (; *value != '\0'; ++value) {
        const auto byte = static_cast<unsigned char>(*value);
        if (byte < 0x20 || byte == 0x7f) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool valid_https_url(const char *url) {
    if (url == nullptr) {
        return false;
    }
    const std::size_t size = std::strlen(url);
    constexpr char prefix[] = "https://";
    if (size <= sizeof(prefix) - 1 || size > max_http_url_bytes ||
        std::memcmp(url, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    const char *authority = url + sizeof(prefix) - 1;
    const char *authority_end = std::strpbrk(authority, "/?#");
    if (authority_end == nullptr) {
        authority_end = url + size;
    }
    if (authority == authority_end || std::memchr(
            authority, '@', static_cast<std::size_t>(authority_end - authority)) !=
            nullptr) {
        return false;
    }
    if (std::strchr(url, '#') != nullptr || contains_control(url)) {
        return false;
    }
    const char *host_end = authority_end;
    const char *port = static_cast<const char *>(std::memchr(
        authority, ':', static_cast<std::size_t>(authority_end - authority)));
    if (port != nullptr) {
        if (authority_end - port != 4 || std::memcmp(port, ":443", 4) != 0 ||
            port == authority) {
            return false;
        }
        host_end = port;
    }
    if (*authority == '.' || host_end[-1] == '.' || *authority == '[') {
        return false;
    }
    bool label_has_byte = false;
    bool host_is_numeric = true;
    std::size_t label_size = 0;
    for (const char *cursor = authority; cursor != host_end; ++cursor) {
        const auto byte = static_cast<unsigned char>(*cursor);
        if (*cursor == '.') {
            if (!label_has_byte || cursor[-1] == '-') {
                return false;
            }
            label_has_byte = false;
            label_size = 0;
        } else if (std::isalnum(byte) || *cursor == '-') {
            if (!label_has_byte && *cursor == '-') {
                return false;
            }
            label_has_byte = true;
            ++label_size;
            if (label_size > 63) {
                return false;
            }
            if (!std::isdigit(byte)) {
                host_is_numeric = false;
            }
        } else {
            return false;
        }
    }
    if (!label_has_byte || host_end[-1] == '-') {
        return false;
    }
    const std::size_t host_size = static_cast<std::size_t>(host_end - authority);
    if (host_size > 253 || host_is_numeric ||
        text_equal_fold(authority, host_size, "localhost") ||
        (host_size > 6 && text_equal_fold(host_end - 6, 6, ".local"))) {
        return false;
    }
    return true;
}

bool same_https_origin(const char *left, const char *right) {
    if (!valid_https_url(left) || !valid_https_url(right)) {
        return false;
    }
    constexpr std::size_t prefix_size = sizeof("https://") - 1;
    const char *left_authority = left + prefix_size;
    const char *right_authority = right + prefix_size;
    const char *left_end = std::strpbrk(left_authority, "/?");
    const char *right_end = std::strpbrk(right_authority, "/?");
    if (left_end == nullptr) {
        left_end = left + std::strlen(left);
    }
    if (right_end == nullptr) {
        right_end = right + std::strlen(right);
    }
    if (left_end - left_authority >= 4 &&
        std::memcmp(left_end - 4, ":443", 4) == 0) {
        left_end -= 4;
    }
    if (right_end - right_authority >= 4 &&
        std::memcmp(right_end - 4, ":443", 4) == 0) {
        right_end -= 4;
    }
    const std::size_t left_size =
        static_cast<std::size_t>(left_end - left_authority);
    const std::size_t right_size =
        static_cast<std::size_t>(right_end - right_authority);
    if (left_size != right_size) {
        return false;
    }
    for (std::size_t index = 0; index < left_size; ++index) {
        if (!ascii_equal_fold(left_authority[index], right_authority[index])) {
            return false;
        }
    }
    return true;
}

bool valid_header_name(const char *name) {
    if (name == nullptr) {
        return false;
    }
    const std::size_t size = std::strlen(name);
    if (size == 0 || size > max_http_header_name_bytes) {
        return false;
    }
    for (const char *cursor = name; *cursor != '\0'; ++cursor) {
        const auto byte = static_cast<unsigned char>(*cursor);
        if (!std::isalnum(byte) && *cursor != '-') {
            return false;
        }
    }
    return true;
}

bool valid_header_value(const char *value) {
    return value != nullptr && std::strlen(value) <= max_http_header_value_bytes &&
           !contains_control(value);
}

bool header_allowed_on_redirect(const char *name) {
    if (!valid_header_name(name)) {
        return false;
    }
    const std::size_t size = std::strlen(name);
    return !text_equal_fold(name, size, "Authorization") &&
           !text_equal_fold(name, size, "Cookie") &&
           !text_equal_fold(name, size, "X-Subscription-Token");
}

bool content_type_matches(const char *actual, const char *expected) {
    if (actual == nullptr || expected == nullptr || *expected == '\0') {
        return false;
    }
    std::size_t index = 0;
    for (; expected[index] != '\0'; ++index) {
        if (actual[index] == '\0' || !ascii_equal_fold(actual[index], expected[index])) {
            return false;
        }
    }
    while (actual[index] == ' ' || actual[index] == '\t') {
        ++index;
    }
    return actual[index] == '\0' || actual[index] == ';';
}

agent::Error map_http_status(int status) {
    if (status >= 200 && status < 300) {
        return agent::Error::none;
    }
    if (status == 401 || status == 403) {
        return agent::Error::authentication;
    }
    if (status == 402) {
        return agent::Error::payment_required;
    }
    if (status == 408 || status == 429) {
        return agent::Error::rate_limited;
    }
    if (status >= 500 && status < 600) {
        return agent::Error::server_error;
    }
    return agent::Error::malformed_response;
}

bool is_redirect_status(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 ||
           status == 308;
}

agent::Error validate_response_head(
    int status, const char *content_type, std::int64_t content_length,
    const ResponsePolicy &policy) {
    const agent::Error status_error = map_http_status(status);
    if (status_error != agent::Error::none) {
        return status_error;
    }
    if (policy.max_response_bytes == 0 ||
        policy.max_response_bytes > max_http_response_bytes) {
        return agent::Error::invalid_argument;
    }
    if (content_length >= 0 &&
        static_cast<std::uint64_t>(content_length) >
            policy.max_response_bytes) {
        return agent::Error::response_too_large;
    }
    if (policy.accepted_content_type_count == 0 ||
        policy.accepted_content_type_count > max_accepted_content_types ||
        policy.accepted_content_types == nullptr) {
        return agent::Error::unsupported_media;
    }
    for (std::size_t index = 0; index < policy.accepted_content_type_count;
         ++index) {
        if (content_type_matches(
                content_type, policy.accepted_content_types[index])) {
            return agent::Error::none;
        }
    }
    return agent::Error::unsupported_media;
}

agent::Error validate_transfer_limits(
    std::size_t body_bytes, std::size_t request_limit,
    std::size_t response_limit) {
    if (request_limit == 0 || response_limit == 0 ||
        request_limit > max_http_request_bytes ||
        response_limit > max_http_response_bytes) {
        return agent::Error::invalid_argument;
    }
    return body_bytes > request_limit ? agent::Error::request_too_large
                                      : agent::Error::none;
}

agent::Error validate_response_completion(
    std::int64_t content_length, std::size_t received_bytes,
    bool client_reports_complete) {
    if (!client_reports_complete) {
        return agent::Error::disconnected;
    }
    if (content_length >= 0 &&
        static_cast<std::uint64_t>(content_length) != received_bytes) {
        return agent::Error::disconnected;
    }
    return agent::Error::none;
}

bool ResponseBudget::accept(std::size_t bytes) {
    if (bytes > limit_ - used_) {
        return false;
    }
    used_ += bytes;
    return true;
}

}  // namespace transport
}  // namespace chatesp
