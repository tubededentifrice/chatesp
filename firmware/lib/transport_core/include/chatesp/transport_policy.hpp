#pragma once

#include <cstddef>
#include <cstdint>

#include "chatesp/agent_types.hpp"

namespace chatesp {
namespace transport {

constexpr std::size_t max_http_url_bytes = agent::Limits::max_url_bytes;
constexpr std::size_t max_http_headers = 10;
constexpr std::size_t max_http_header_name_bytes = 40;
constexpr std::size_t max_http_header_value_bytes = 1'024;
constexpr std::size_t max_http_request_bytes =
    agent::Limits::max_audio_file_bytes + 2'048;
constexpr std::size_t max_http_response_bytes =
    agent::Limits::max_tts_pcm_bytes;
constexpr std::size_t max_accepted_content_types = 4;

enum class HttpMethod : std::uint8_t { get, post };

struct ResponsePolicy {
    const char *const *accepted_content_types = nullptr;
    std::size_t accepted_content_type_count = 0;
    std::size_t max_response_bytes = 0;
};

[[nodiscard]] bool valid_https_url(const char *url);
[[nodiscard]] bool valid_header_name(const char *name);
[[nodiscard]] bool valid_header_value(const char *value);
[[nodiscard]] bool header_allowed_on_redirect(const char *name);
[[nodiscard]] bool content_type_matches(
    const char *actual, const char *expected);
[[nodiscard]] agent::Error map_http_status(int status);
[[nodiscard]] bool is_redirect_status(int status);
[[nodiscard]] agent::Error validate_response_head(
    int status, const char *content_type, std::int64_t content_length,
    const ResponsePolicy &policy);
[[nodiscard]] agent::Error validate_transfer_limits(
    std::size_t body_bytes, std::size_t request_limit,
    std::size_t response_limit);
[[nodiscard]] agent::Error validate_response_completion(
    std::int64_t content_length, std::size_t received_bytes,
    bool client_reports_complete);

class ResponseBudget {
public:
    explicit ResponseBudget(std::size_t limit) : limit_(limit) {}

    [[nodiscard]] bool accept(std::size_t bytes);
    [[nodiscard]] std::size_t used() const { return used_; }
    [[nodiscard]] std::size_t remaining() const { return limit_ - used_; }

private:
    std::size_t limit_ = 0;
    std::size_t used_ = 0;
};

class ElapsedTimer {
public:
    explicit ElapsedTimer(std::uint32_t start_ms) : start_ms_(start_ms) {}

    [[nodiscard]] bool expired(
        std::uint32_t now_ms, std::uint32_t limit_ms) const {
        return elapsed_ms(now_ms) >= limit_ms;
    }
    [[nodiscard]] std::uint32_t elapsed_ms(std::uint32_t now_ms) const {
        return static_cast<std::uint32_t>(now_ms - start_ms_);
    }

private:
    std::uint32_t start_ms_ = 0;
};

}  // namespace transport
}  // namespace chatesp
