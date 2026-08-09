#pragma once

#include <cstddef>
#include <cstdint>

namespace chatesp {
namespace agent {

struct Limits {
    static constexpr std::size_t max_recording_pcm_bytes = 960'000;
    static constexpr std::size_t max_audio_file_bytes =
        max_recording_pcm_bytes + 44;
    static constexpr std::size_t max_transcript_bytes = 2'048;
    static constexpr std::size_t max_answer_bytes = 1'280;
    static constexpr std::size_t max_tts_input_bytes = 640;
    static constexpr std::size_t max_tts_first_request_bytes = 160;
    static constexpr std::size_t max_tts_segment_bytes = max_tts_input_bytes;
    static constexpr std::size_t max_tts_segments = 2;
    static constexpr std::size_t max_message_bytes = 4'096;
    static constexpr std::size_t max_history_messages = 12;
    static constexpr std::size_t max_chat_request_bytes = 32'768;
    static constexpr std::size_t max_chat_response_bytes = 128'000;
    static constexpr std::size_t max_sse_line_bytes = 16'384;
    static constexpr std::size_t max_tool_count = 12;
    static constexpr std::size_t max_tool_rounds = 3;
    static constexpr std::size_t max_tool_name_bytes = 32;
    static constexpr std::size_t max_tool_description_bytes = 192;
    static constexpr std::size_t max_tool_schema_bytes = 768;
    static constexpr std::size_t max_tool_call_id_bytes = 96;
    static constexpr std::size_t max_python_source_bytes = 1'024;
    static constexpr std::size_t max_tool_arguments_bytes =
        max_python_source_bytes * 6 + 32;
    static constexpr std::size_t max_tool_result_bytes = 4'096;
    // These two values are the firmware source of truth for memory capacity.
    // Record buffers, tool schemas, and model prompt text derive from them.
    static constexpr std::size_t max_memory_facts = 10;
    static constexpr std::size_t max_memory_fact_bytes = 128;
    static constexpr std::size_t max_python_output_bytes = 2'048;
    static constexpr std::size_t max_plot_points = 128;
    static constexpr std::size_t max_plot_title_bytes = 48;
    static constexpr std::size_t python_heap_bytes = 256 * 1'024;
    static constexpr std::size_t python_stack_limit_bytes = 12 * 1'024;
    static constexpr std::uint32_t python_maximum_duration_ms = 1'000;
    static constexpr std::uint32_t python_maximum_vm_hooks = 250'000;
    static constexpr std::size_t max_search_query_bytes = 200;
    static constexpr std::size_t max_web_response_bytes = 64'000;
    static constexpr std::size_t max_image_search_response_bytes = 96'000;
    static constexpr std::size_t max_web_results = 5;
    static constexpr std::size_t max_image_results = 6;
    static constexpr std::size_t max_title_bytes = 160;
    static constexpr std::size_t max_url_bytes = 768;
    static constexpr std::size_t max_snippet_bytes = 384;
    static constexpr std::size_t max_image_id_bytes = 16;
    static constexpr std::size_t max_image_download_bytes = 768 * 1'024;
    static constexpr std::uint32_t max_image_dimension = 2'048;
    static constexpr std::uint8_t max_image_redirects = 2;
    static constexpr std::size_t max_tts_pcm_bytes = 2'160'000;
    static constexpr std::uint16_t max_chat_output_tokens = 160;
};

static_assert(
    Limits::max_memory_facts >= 2,
    "Memory compaction needs space for a pending fact");
static_assert(
    Limits::max_memory_facts <= 255,
    "The memory record stores its count in one byte");
static_assert(
    Limits::max_memory_fact_bytes >= 1,
    "A memory fact must have a positive byte limit");
static_assert(
    Limits::max_memory_fact_bytes <= 65'535,
    "The memory record stores fact length in two bytes");

struct RequestPolicy {
    std::uint32_t connect_timeout_ms = 5'000;
    std::uint32_t first_byte_timeout_ms = 20'000;
    std::uint32_t idle_timeout_ms = 10'000;
    std::uint32_t total_timeout_ms = 45'000;
    std::uint8_t max_attempts = 2;
};

constexpr RequestPolicy chat_policy() {
    return {20'000, 30'000, 15'000, 75'000, 2};
}
constexpr RequestPolicy transcription_policy() {
    return {20'000, 30'000, 15'000, 75'000, 2};
}
constexpr RequestPolicy speech_policy() {
    return {20'000, 40'000, 25'000, 90'000, 2};
}
constexpr RequestPolicy search_policy() {
    return {10'000, 12'000, 8'000, 20'000, 2};
}
constexpr RequestPolicy image_fetch_policy() {
    return {5'000, 8'000, 5'000, 20'000, 2};
}

}  // namespace agent
}  // namespace chatesp
