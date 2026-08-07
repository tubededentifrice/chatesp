#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <unity.h>

#include "chatesp/provider_helpers.hpp"

using chatesp::agent::AudioView;
using chatesp::agent::Error;
using chatesp::agent::FixedText;
using chatesp::agent::Limits;
using namespace chatesp::provider;

namespace {

void assert_error(Error expected, Error actual) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual));
}

std::uint32_t read_u32(const std::uint8_t *data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_secret_header_is_bounded_and_rejects_controls() {
    char output[32]{};
    const char key[] = "safe-key_123";
    assert_error(
        Error::none,
        build_bearer_header(
            {key, sizeof(key) - 1}, output, sizeof(output)));
    TEST_ASSERT_EQUAL_STRING("Bearer safe-key_123", output);

    const char bad[] = "key\r\nInjected: value";
    TEST_ASSERT_FALSE(valid_secret({bad, sizeof(bad) - 1}, 64));
    assert_error(
        Error::invalid_argument,
        build_bearer_header({key, sizeof(key) - 1}, output, 4));
    assert_error(
        Error::invalid_argument,
        build_bearer_header({key, sizeof(key) - 1}, output, 0));
}

void test_api_url_uses_configured_https_base_and_fixed_path() {
    FixedText<Limits::max_url_bytes> url;
    const char base[] = "https://router.example.test/api/v1/";
    assert_error(
        Error::none,
        build_api_url(
            base, sizeof(base) - 1, "/chat/completions", url));
    TEST_ASSERT_EQUAL_STRING(
        "https://router.example.test/api/v1/chat/completions",
        url.c_str());

    const char query_base[] = "https://router.example.test/api?key=x";
    assert_error(
        Error::invalid_argument,
        build_api_url(
            query_base, sizeof(query_base) - 1, "/chat/completions", url));
    const char userinfo[] = "https://user@router.example.test/api";
    assert_error(
        Error::invalid_argument,
        build_api_url(
            userinfo, sizeof(userinfo) - 1, "/chat/completions", url));
    const char insecure[] = "http://router.example.test/api";
    assert_error(
        Error::invalid_argument,
        build_api_url(
            insecure, sizeof(insecure) - 1, "/chat/completions", url));
    assert_error(
        Error::invalid_argument,
        build_api_url(
            base, sizeof(base) - 1, "/../secret", url));
}

void test_api_url_allows_encoded_search_query_in_generated_path() {
    FixedText<Limits::max_url_bytes> url;
    const char base[] = "https://api.search.example.test";
    const char target[] = "/search?q=Dubai%20weather...&count=5";
    assert_error(
        Error::none,
        build_api_url(
            base, sizeof(base) - 1, target, url));
    TEST_ASSERT_EQUAL_STRING(
        "https://api.search.example.test/search?q=Dubai%20weather...&count=5",
        url.c_str());
}

void test_wav_stream_has_exact_header_and_segment_order() {
    const std::array<std::uint8_t, 2> prefix = {'P', ':'};
    std::array<std::uint8_t, 6> pcm = {1, 2, 3, 4, 5, 6};
    const std::array<std::uint8_t, 2> suffix = {';', 'S'};
    AudioView audio{pcm.data(), pcm.size(), 16'000, 1, 16};
    WavPcmStream stream;
    assert_error(
        Error::none,
        stream.configure(
            prefix.data(), prefix.size(), audio, suffix.data(),
            suffix.size()));
    TEST_ASSERT_EQUAL_UINT32(54, stream.size());
    TEST_ASSERT_EQUAL_MEMORY("RIFF", stream.wav_header().data(), 4);
    TEST_ASSERT_EQUAL_UINT32(42, read_u32(stream.wav_header().data() + 4));
    TEST_ASSERT_EQUAL_UINT32(16'000, read_u32(stream.wav_header().data() + 24));
    TEST_ASSERT_EQUAL_UINT32(32'000, read_u32(stream.wav_header().data() + 28));
    TEST_ASSERT_EQUAL_UINT32(6, read_u32(stream.wav_header().data() + 40));

    std::array<std::uint8_t, 54> output{};
    std::size_t offset = 0;
    while (offset < output.size()) {
        std::size_t read = 0;
        assert_error(
            Error::none,
            stream.read(output.data() + offset, 3, read));
        TEST_ASSERT_TRUE(read > 0);
        offset += read;
    }
    TEST_ASSERT_EQUAL_MEMORY(prefix.data(), output.data(), prefix.size());
    TEST_ASSERT_EQUAL_MEMORY("RIFF", output.data() + 2, 4);
    TEST_ASSERT_EQUAL_MEMORY(pcm.data(), output.data() + 46, pcm.size());
    TEST_ASSERT_EQUAL_MEMORY(suffix.data(), output.data() + 52, suffix.size());
    std::size_t read = 99;
    assert_error(Error::none, stream.read(output.data(), 3, read));
    TEST_ASSERT_EQUAL_UINT32(0, read);
}

void test_wav_stream_reads_original_pcm_without_a_second_copy() {
    std::array<std::uint8_t, 2> pcm = {7, 8};
    AudioView audio{pcm.data(), pcm.size(), 16'000, 1, 16};
    WavPcmStream stream;
    assert_error(
        Error::none,
        stream.configure(nullptr, 0, audio, nullptr, 0));
    pcm[0] = 9;
    std::array<std::uint8_t, 46> output{};
    std::size_t read = 0;
    assert_error(
        Error::none,
        stream.read(output.data(), output.size(), read));
    TEST_ASSERT_EQUAL_UINT32(output.size(), read);
    TEST_ASSERT_EQUAL_UINT8(9, output[44]);
    TEST_ASSERT_EQUAL_UINT8(8, output[45]);
}

void test_wav_stream_rejects_wrong_format_and_odd_pcm() {
    std::array<std::uint8_t, 4> pcm{};
    WavPcmStream stream;
    AudioView wrong_rate{pcm.data(), pcm.size(), 24'000, 1, 16};
    assert_error(
        Error::invalid_argument,
        stream.configure(nullptr, 0, wrong_rate, nullptr, 0));
    AudioView odd{pcm.data(), 3, 16'000, 1, 16};
    assert_error(
        Error::invalid_argument,
        stream.configure(nullptr, 0, odd, nullptr, 0));
}

void test_response_buffer_keeps_terminator_and_stops_overflow() {
    std::array<char, 6> storage{};
    BoundedResponseBuffer buffer(storage.data(), 5);
    const std::uint8_t first[] = {'1', '2', '3'};
    const std::uint8_t second[] = {'4', '5'};
    const std::uint8_t extra[] = {'6'};
    TEST_ASSERT_TRUE(buffer.append(first, sizeof(first)));
    TEST_ASSERT_TRUE(buffer.append(second, sizeof(second)));
    TEST_ASSERT_EQUAL_STRING("12345", buffer.data());
    TEST_ASSERT_FALSE(buffer.append(extra, sizeof(extra)));
    TEST_ASSERT_EQUAL_UINT32(5, buffer.size());
    TEST_ASSERT_EQUAL_UINT8(0, storage[5]);
    buffer.reset();
    TEST_ASSERT_EQUAL_UINT32(0, buffer.size());
    TEST_ASSERT_EQUAL_STRING("", buffer.data());
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_secret_header_is_bounded_and_rejects_controls);
    RUN_TEST(test_api_url_uses_configured_https_base_and_fixed_path);
    RUN_TEST(test_api_url_allows_encoded_search_query_in_generated_path);
    RUN_TEST(test_wav_stream_has_exact_header_and_segment_order);
    RUN_TEST(test_wav_stream_reads_original_pcm_without_a_second_copy);
    RUN_TEST(test_wav_stream_rejects_wrong_format_and_odd_pcm);
    RUN_TEST(test_response_buffer_keeps_terminator_and_stops_overflow);
    return UNITY_END();
}
