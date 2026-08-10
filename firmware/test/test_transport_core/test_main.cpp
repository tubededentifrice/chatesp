#include <cstdint>

#include <unity.h>

#include "chatesp/transport_policy.hpp"

using chatesp::agent::Error;
using namespace chatesp::transport;

namespace {

void assert_error(Error expected, Error actual) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual));
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_https_url_rejects_insecure_credentials_fragments_and_controls() {
    TEST_ASSERT_TRUE(valid_https_url("https://api.example.test/v1?q=a"));
    TEST_ASSERT_FALSE(valid_https_url("http://api.example.test/v1"));
    TEST_ASSERT_FALSE(valid_https_url("https://user@api.example.test/v1"));
    TEST_ASSERT_FALSE(valid_https_url("https://api.example.test/v1#token"));
    TEST_ASSERT_FALSE(valid_https_url("https://api.example.test/\rnext"));
    TEST_ASSERT_FALSE(valid_https_url("https://localhost/image.jpg"));
    TEST_ASSERT_FALSE(valid_https_url("https://LOCALHOST/image.jpg"));
    TEST_ASSERT_FALSE(valid_https_url("https://device.local/image.jpg"));
    TEST_ASSERT_FALSE(valid_https_url("https://127.0.0.1/image.jpg"));
    TEST_ASSERT_FALSE(valid_https_url("https://api.example.test:444/v1"));
    TEST_ASSERT_TRUE(valid_https_url("https://api.example.test:443/v1"));
}

void test_header_validation_rejects_injection() {
    TEST_ASSERT_TRUE(valid_header_name("Authorization"));
    TEST_ASSERT_FALSE(valid_header_name("Bad Header"));
    TEST_ASSERT_TRUE(valid_header_value("Bearer redacted"));
    TEST_ASSERT_FALSE(valid_header_value("value\r\nInjected: yes"));
    TEST_ASSERT_FALSE(header_allowed_on_redirect("Authorization"));
    TEST_ASSERT_FALSE(header_allowed_on_redirect("cookie"));
    TEST_ASSERT_TRUE(header_allowed_on_redirect("Accept"));
}

void test_persistent_sessions_are_bound_to_one_https_origin() {
    TEST_ASSERT_TRUE(same_https_origin(
        "https://api.example.test/v1/chat",
        "https://API.example.test:443/v1/audio"));
    TEST_ASSERT_FALSE(same_https_origin(
        "https://api.example.test/v1/chat",
        "https://images.example.test/file.jpg"));
    TEST_ASSERT_FALSE(same_https_origin(
        "https://api.example.test/v1/chat",
        "http://api.example.test/v1/audio"));
}

void test_transfer_limits_have_absolute_and_request_caps() {
    assert_error(Error::none, validate_transfer_limits(100, 100, 200));
    assert_error(
        Error::request_too_large,
        validate_transfer_limits(101, 100, 200));
    assert_error(
        Error::invalid_argument,
        validate_transfer_limits(0, max_http_request_bytes + 1, 200));
    assert_error(
        Error::invalid_argument,
        validate_transfer_limits(0, 100, max_http_response_bytes + 1));
}

void test_content_type_is_case_insensitive_and_allows_parameters() {
    TEST_ASSERT_TRUE(content_type_matches(
        "Application/JSON; charset=utf-8", "application/json"));
    TEST_ASSERT_TRUE(content_type_matches(
        "IMAGE/JPEG ; charset=binary", "image/jpeg"));
    TEST_ASSERT_FALSE(content_type_matches("text/html", "application/json"));
    TEST_ASSERT_FALSE(content_type_matches("application/jsonp", "application/json"));
}

void test_response_head_checks_status_media_and_declared_size() {
    const char *types[] = {"application/json"};
    const ResponsePolicy policy{types, 1, 100};
    assert_error(
        Error::none,
        validate_response_head(200, "application/json", 100, policy));
    assert_error(
        Error::response_too_large,
        validate_response_head(200, "application/json", 101, policy));
    assert_error(
        Error::unsupported_media,
        validate_response_head(200, "text/html", 20, policy));
    assert_error(
        Error::authentication,
        validate_response_head(401, "application/json", 20, policy));
    assert_error(
        Error::server_error,
        validate_response_head(503, "application/json", 20, policy));
    const ResponsePolicy invalid_policy{types, 1, 0};
    assert_error(
        Error::invalid_argument,
        validate_response_head(
            200, "application/json", 20, invalid_policy));
}

void test_response_budget_stops_chunked_overflow() {
    ResponseBudget budget{10};
    TEST_ASSERT_TRUE(budget.accept(6));
    TEST_ASSERT_TRUE(budget.accept(4));
    TEST_ASSERT_FALSE(budget.accept(1));
    TEST_ASSERT_EQUAL_UINT32(10, budget.used());
}

void test_response_completion_rejects_truncated_bodies() {
    assert_error(
        Error::none, validate_response_completion(10, 10, true));
    assert_error(
        Error::none, validate_response_completion(-1, 10, true));
    assert_error(
        Error::disconnected, validate_response_completion(10, 9, true));
    assert_error(
        Error::disconnected, validate_response_completion(-1, 10, false));
}

void test_elapsed_timer_handles_millisecond_wrap() {
    ElapsedTimer timer{0xfffffff0U};
    TEST_ASSERT_FALSE(timer.expired(0x00000003U, 20));
    TEST_ASSERT_TRUE(timer.expired(0x00000004U, 20));
}

void test_status_and_redirect_policy_is_narrow() {
    TEST_ASSERT_TRUE(is_redirect_status(308));
    TEST_ASSERT_FALSE(is_redirect_status(304));
    assert_error(Error::rate_limited, map_http_status(429));
    assert_error(Error::payment_required, map_http_status(402));
    assert_error(Error::malformed_response, map_http_status(404));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_https_url_rejects_insecure_credentials_fragments_and_controls);
    RUN_TEST(test_header_validation_rejects_injection);
    RUN_TEST(test_persistent_sessions_are_bound_to_one_https_origin);
    RUN_TEST(test_transfer_limits_have_absolute_and_request_caps);
    RUN_TEST(test_content_type_is_case_insensitive_and_allows_parameters);
    RUN_TEST(test_response_head_checks_status_media_and_declared_size);
    RUN_TEST(test_response_budget_stops_chunked_overflow);
    RUN_TEST(test_response_completion_rejects_truncated_bodies);
    RUN_TEST(test_elapsed_timer_handles_millisecond_wrap);
    RUN_TEST(test_status_and_redirect_policy_is_narrow);
    return UNITY_END();
}
