#pragma once

#include <cstddef>

#include "chatesp/tool_registry.hpp"

namespace chatesp {
namespace agent {

struct OpenRouterConfig {
    const char *chat_model = "~deepseek/deepseek-v4-flash-latest";
    const char *transcription_model = "openai/whisper-large-v3-turbo";
    const char *speech_model = "hexgrad/kokoro-82m";
    const char *speech_voice = "af_heart";
    const char *french_speech_voice = "ff_siwis";
};

using ChatRequestBody = FixedText<Limits::max_chat_request_bytes>;
using SpeechRequestBody = FixedText<2'048>;

struct MultipartTranscriptionPlan {
    FixedText<768> preamble;
    FixedText<80> epilogue;
    FixedText<96> content_type;
    std::size_t content_length = 0;
};

Error build_openrouter_chat_request(
    const OpenRouterConfig &config, const ConversationHistory &history,
    const ToolRegistry &tools, const MemorySnapshot &memories,
    const char *approximate_location,
    std::size_t approximate_location_size, const char *current_utc_minute,
    bool stream, ChatRequestBody &body);

Error build_openrouter_route_request(
    const OpenRouterConfig &config, const ConversationHistory &history,
    const ToolRegistry &tools, const MemorySnapshot &memories,
    const char *approximate_location,
    std::size_t approximate_location_size, const char *current_utc_minute,
    bool stream, ChatRequestBody &body);

Error build_openrouter_answer_request(
    const OpenRouterConfig &config, const ConversationHistory &history,
    const MemorySnapshot &memories, const char *approximate_location,
    std::size_t approximate_location_size,
    const char *current_utc_minute, bool stream, ChatRequestBody &body);

Error build_openrouter_transcription_plan(
    const OpenRouterConfig &config, std::size_t audio_file_bytes,
    MultipartTranscriptionPlan &plan);

Error build_openrouter_speech_request(
    const OpenRouterConfig &config, const char *text, std::size_t size,
    SpeechRequestBody &body);

OpenRouterConfig openrouter_speech_config_for_language(
    const OpenRouterConfig &config, SpeechLanguage language);

Error parse_openrouter_chat_response(
    const char *json, std::size_t size, ChatTurn &turn);

Error parse_openrouter_transcription_response(
    const char *json, std::size_t size,
    FixedText<Limits::max_transcript_bytes> &transcript);

Error validate_openrouter_pcm_content_type(const char *content_type);

class OpenRouterSseParser {
public:
    explicit OpenRouterSseParser(ChatTextSink *text_sink = nullptr)
        : text_sink_(text_sink) {}

    Error feed(const char *data, std::size_t size);
    Error finish();
    void reset();
    void set_text_sink(ChatTextSink *text_sink) { text_sink_ = text_sink; }

    [[nodiscard]] bool done() const { return done_; }
    [[nodiscard]] const ChatTurn &turn() const { return turn_; }
    [[nodiscard]] Error error() const { return error_; }
    [[nodiscard]] bool output_started() const {
        return published_size_ != 0;
    }

private:
    Error process_line();

    FixedText<Limits::max_sse_line_bytes> line_;
    ChatTurn raw_turn_;
    ChatTurn turn_;
    Error error_ = Error::none;
    ChatTextSink *text_sink_ = nullptr;
    std::size_t published_size_ = 0;
    bool language_resolved_ = false;
    bool language_published_ = false;
    bool saw_finish_ = false;
    bool done_ = false;
};

}  // namespace agent
}  // namespace chatesp
