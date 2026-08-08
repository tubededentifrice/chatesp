#include "chatesp/openrouter_protocol.hpp"

#include <cstring>
#include <limits>

#include "chatesp/system_prompt.hpp"
#include "chatesp/utc_clock.hpp"
#include "json.hpp"

namespace chatesp {
namespace agent {
namespace {

constexpr char multipart_boundary[] = "ChatESPBoundary7MA4YWxk";
constexpr char kokoro_model[] = "hexgrad/kokoro-82m";
constexpr char kokoro_french_voice[] = "ff_siwis";

template <std::size_t Capacity>
bool append_key(FixedText<Capacity> &output, const char *key) {
    return detail::append_json_string(output, key) && output.push_back(':');
}

template <std::size_t Capacity>
bool append_message(FixedText<Capacity> &body, const Message &message) {
    if (!body.push_back('{') || !append_key(body, "role")) return false;
    const char *role = "user";
    if (message.role == MessageRole::assistant ||
        message.role == MessageRole::assistant_tool_call) {
        role = "assistant";
    } else if (message.role == MessageRole::tool) {
        role = "tool";
    }
    if (!detail::append_json_string(body, role)) return false;

    if (message.role == MessageRole::assistant_tool_call) {
        return body.append(",\"content\":null,\"tool_calls\":[{") &&
               append_key(body, "type") &&
               detail::append_json_string(body, "function") &&
               body.push_back(',') && append_key(body, "id") &&
               detail::append_json_string(
                   body, message.tool_call.id.data(),
                   message.tool_call.id.size()) &&
               body.append(",\"function\":{") && append_key(body, "name") &&
               detail::append_json_string(
                   body, message.tool_call.name.data(),
                   message.tool_call.name.size()) &&
               body.push_back(',') && append_key(body, "arguments") &&
               detail::append_json_string(
                   body, message.tool_call.arguments.data(),
                   message.tool_call.arguments.size()) &&
               body.append("}}]}");
    }

    if (message.role == MessageRole::tool) {
        return body.push_back(',') && append_key(body, "tool_call_id") &&
               detail::append_json_string(
                   body, message.tool_call.id.data(),
                   message.tool_call.id.size()) &&
               body.push_back(',') && append_key(body, "name") &&
               detail::append_json_string(
                   body, message.tool_call.name.data(),
                   message.tool_call.name.size()) &&
               body.push_back(',') && append_key(body, "content") &&
               detail::append_json_string(
                   body, message.content.data(), message.content.size()) &&
               body.push_back('}');
    }

    return body.push_back(',') && append_key(body, "content") &&
           detail::append_json_string(
               body, message.content.data(), message.content.size()) &&
           body.push_back('}');
}

bool valid_config_token(const char *text, std::size_t max_size = 128) {
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    std::size_t index = 0;
    for (; index <= max_size && text[index] != '\0'; ++index) {
        const unsigned char value = static_cast<unsigned char>(text[index]);
        if (!((value >= 'A' && value <= 'Z') ||
              (value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '-' || value == '_' ||
              value == '.' || value == '/' || value == '~')) {
            return false;
        }
    }
    return index <= max_size;
}

template <std::size_t Capacity>
bool append_system_message(
    FixedText<Capacity> &body, const char *prompt,
    const char *approximate_location, std::size_t approximate_location_size,
    const char *current_utc_minute) {
    FixedText<2'048> content;
    return UtcClock::valid_minute_text(current_utc_minute) &&
        content.assign(prompt) &&
        content.append(" Approximate user location: ") &&
        (approximate_location_size == 0
             ? content.append("not provided")
             : content.append(
                   approximate_location, approximate_location_size)) &&
        content.append(
            ". Use this location only for requests that depend on location.") &&
        content.append(" Current user date and time: ") &&
        content.append(current_utc_minute) && content.push_back('.') &&
        detail::append_json_string(body, content.data(), content.size());
}

bool valid_approximate_location(const char *value, std::size_t size) {
    if (size > 96 || (size != 0 && value == nullptr)) {
        return false;
    }
    for (std::size_t index = 0; index < size; ++index) {
        const auto byte = static_cast<unsigned char>(value[index]);
        if (byte < 0x20U || byte == 0x7fU) {
            return false;
        }
    }
    return true;
}

enum class LanguageTagState : std::uint8_t {
    pending,
    ready,
    malformed,
};

bool starts_with(const char *text, std::size_t size, const char *prefix,
                 std::size_t prefix_size) {
    return size >= prefix_size &&
        std::memcmp(text, prefix, prefix_size) == 0;
}

bool is_prefix_of(const char *text, std::size_t size, const char *value,
                  std::size_t value_size) {
    return size <= value_size && std::memcmp(text, value, size) == 0;
}

LanguageTagState filter_answer_language(
    const FixedText<Limits::max_answer_bytes> &raw,
    FixedText<Limits::max_answer_bytes> &answer,
    SpeechLanguage &language) {
    constexpr char english_tag[] = "[[lang=en]]";
    constexpr char french_tag[] = "[[lang=fr]]";
    constexpr char tag_prefix[] = "[[lang=";
    constexpr std::size_t tag_size = sizeof(english_tag) - 1;

    if (raw.empty()) {
        return LanguageTagState::pending;
    }
    const char *text = raw.data();
    const std::size_t size = raw.size();
    if (is_prefix_of(text, size, english_tag, tag_size) ||
        is_prefix_of(text, size, french_tag, tag_size)) {
        return LanguageTagState::pending;
    }

    std::size_t answer_offset = 0;
    language = SpeechLanguage::english;
    if (starts_with(text, size, english_tag, tag_size)) {
        answer_offset = tag_size;
    } else if (starts_with(text, size, french_tag, tag_size)) {
        answer_offset = tag_size;
        language = SpeechLanguage::french;
    } else if (starts_with(
                   text, size, tag_prefix, sizeof(tag_prefix) - 1)) {
        return LanguageTagState::malformed;
    }

    if (!answer.assign(text + answer_offset, size - answer_offset)) {
        return LanguageTagState::malformed;
    }
    return LanguageTagState::ready;
}

bool parse_function(
    detail::JsonReader &reader, ToolInvocation &call, bool append) {
    if (!reader.consume('{')) return false;
    if (reader.peek() == '}') return reader.consume('}');
    while (true) {
        FixedText<48> key;
        if (!reader.read_string(key) || !reader.consume(':')) return false;
        if (key.equals("name")) {
            FixedText<Limits::max_tool_name_bytes> value;
            if (!reader.read_string(value)) return false;
            if (append) {
                if (!call.name.append(value.data(), value.size())) return false;
            } else {
                call.name = value;
            }
        } else if (key.equals("arguments")) {
            FixedText<Limits::max_tool_arguments_bytes> value;
            if (!reader.read_string(value)) return false;
            if (append) {
                if (!call.arguments.append(value.data(), value.size())) return false;
            } else {
                call.arguments = value;
            }
        } else if (!reader.skip_value()) {
            return false;
        }
        if (reader.consume('}')) return true;
        if (!reader.consume(',')) return false;
    }
}

bool parse_tool_call(
    detail::JsonReader &reader, ToolInvocation &call, bool append) {
    if (!reader.consume('{')) return false;
    if (reader.peek() == '}') return reader.consume('}');
    while (true) {
        FixedText<48> key;
        if (!reader.read_string(key) || !reader.consume(':')) return false;
        if (key.equals("id")) {
            FixedText<Limits::max_tool_call_id_bytes> value;
            if (!reader.read_string(value)) return false;
            if (append) {
                if (!call.id.append(value.data(), value.size())) return false;
            } else {
                call.id = value;
            }
        } else if (key.equals("function")) {
            if (!parse_function(reader, call, append)) return false;
        } else if (!reader.skip_value()) {
            return false;
        }
        if (reader.consume('}')) return true;
        if (!reader.consume(',')) return false;
    }
}

bool parse_tool_calls(
    detail::JsonReader &reader, ToolInvocation &call, bool append,
    bool &saw_tool) {
    if (!reader.consume('[')) return false;
    if (reader.peek() == ']') return reader.consume(']');
    if (saw_tool && !append) return false;
    if (!parse_tool_call(reader, call, append)) return false;
    saw_tool = true;
    if (reader.consume(']')) return true;
    // The device requests one call at a time and rejects extra calls.
    return false;
}

bool parse_message(
    detail::JsonReader &reader, ChatTurn &turn, bool append,
    bool &saw_tool) {
    if (!reader.consume('{')) return false;
    if (reader.peek() == '}') return reader.consume('}');
    while (true) {
        FixedText<48> key;
        if (!reader.read_string(key) || !reader.consume(':')) return false;
        if (key.equals("content")) {
            if (reader.peek() == 'n') {
                if (!reader.consume_literal("null")) return false;
            } else {
                FixedText<Limits::max_answer_bytes> value;
                if (!reader.read_string(value)) return false;
                if (append) {
                    if (!turn.answer.append(value.data(), value.size())) return false;
                } else {
                    turn.answer = value;
                }
            }
        } else if (key.equals("tool_calls")) {
            if (!parse_tool_calls(reader, turn.tool_call, append, saw_tool)) {
                return false;
            }
        } else if (!reader.skip_value()) {
            return false;
        }
        if (reader.consume('}')) return true;
        if (!reader.consume(',')) return false;
    }
}

bool parse_choice(
    detail::JsonReader &reader, ChatTurn &turn, bool append,
    FixedText<24> &finish_reason, bool &saw_tool) {
    if (!reader.consume('{')) return false;
    if (reader.peek() == '}') return reader.consume('}');
    while (true) {
        FixedText<48> key;
        if (!reader.read_string(key) || !reader.consume(':')) return false;
        if (key.equals("message") || key.equals("delta")) {
            if (!parse_message(reader, turn, append, saw_tool)) return false;
        } else if (key.equals("finish_reason")) {
            if (reader.peek() == 'n') {
                if (!reader.consume_literal("null")) return false;
            } else if (!reader.read_string(finish_reason)) {
                return false;
            }
        } else if (!reader.skip_value()) {
            return false;
        }
        if (reader.consume('}')) return true;
        if (!reader.consume(',')) return false;
    }
}

Error parse_chat_json(
    const char *json, std::size_t size, ChatTurn &turn, bool append,
    bool &saw_finish) {
    detail::JsonReader reader(json, size);
    if (!reader.consume('{')) return Error::malformed_response;
    bool saw_choice = false;
    bool saw_tool = turn.kind == ChatTurnKind::tool_call;
    FixedText<24> finish_reason;
    if (reader.peek() != '}') {
        while (true) {
            FixedText<48> key;
            if (!reader.read_string(key) || !reader.consume(':')) {
                return Error::malformed_response;
            }
            if (key.equals("error")) {
                if (!reader.skip_value()) return Error::malformed_response;
                return Error::model_failed;
            }
            if (key.equals("choices")) {
                if (!reader.consume('[')) return Error::malformed_response;
                if (reader.peek() != ']') {
                    if (!parse_choice(
                            reader, turn, append, finish_reason, saw_tool)) {
                        return Error::malformed_response;
                    }
                    saw_choice = true;
                    if (!reader.consume(']')) return Error::malformed_response;
                } else {
                    reader.consume(']');
                }
            } else if (!reader.skip_value()) {
                return Error::malformed_response;
            }
            if (reader.consume('}')) break;
            if (!reader.consume(',')) return Error::malformed_response;
        }
    } else {
        reader.consume('}');
    }
    if (!reader.finish() || !saw_choice) return Error::malformed_response;
    if (!finish_reason.empty()) {
        if (finish_reason.equals("tool_calls")) {
            if (!saw_tool || turn.tool_call.id.empty() ||
                turn.tool_call.name.empty() || turn.tool_call.arguments.empty() ||
                !detail::valid_json_value(
                    turn.tool_call.arguments.data(),
                    turn.tool_call.arguments.size())) {
                return Error::malformed_response;
            }
            turn.kind = ChatTurnKind::tool_call;
            turn.answer.clear();
        } else if (finish_reason.equals("stop")) {
            if (turn.answer.empty()) return Error::malformed_response;
            turn.kind = ChatTurnKind::answer;
        } else {
            return Error::model_failed;
        }
        saw_finish = true;
    } else if (saw_tool) {
        turn.kind = ChatTurnKind::tool_call;
    }
    return Error::none;
}

}  // namespace

Error build_openrouter_chat_request(
    const OpenRouterConfig &config, const ConversationHistory &history,
    const ToolRegistry &tools, const char *approximate_location,
    std::size_t approximate_location_size, const char *current_utc_minute,
    bool stream, ChatRequestBody &body) {
    body.clear();
    if (!valid_config_token(config.chat_model) ||
        !UtcClock::valid_minute_text(current_utc_minute) ||
        !valid_approximate_location(
            approximate_location, approximate_location_size)) {
        return Error::invalid_argument;
    }
    if (!body.append("{\"model\":") ||
        !detail::append_json_string(body, config.chat_model) ||
        !body.append(",\"messages\":[{\"role\":\"system\",\"content\":") ||
        !append_system_message(
            body, system_prompt, approximate_location,
            approximate_location_size, current_utc_minute) ||
        !body.push_back('}')) {
        return Error::request_too_large;
    }
    for (std::size_t index = 0; index < history.size(); ++index) {
        if (!body.push_back(',') || !append_message(body, history.at(index))) {
            body.clear();
            return Error::request_too_large;
        }
    }
    if (!body.append("],\"tools\":[")) return Error::request_too_large;
    for (std::size_t index = 0; index < tools.size(); ++index) {
        const Tool &tool = tools.at(index);
        if ((index != 0 && !body.push_back(',')) ||
            !body.append("{\"type\":\"function\",\"function\":{") ||
            !append_key(body, "name") ||
            !detail::append_json_string(body, tool.name()) ||
            !body.push_back(',') || !append_key(body, "description") ||
            !detail::append_json_string(body, tool.description()) ||
            !body.append(",\"parameters\":") ||
            !body.append(tool.parameters_schema()) || !body.append("}}")) {
            body.clear();
            return Error::request_too_large;
        }
    }
    if (!body.append(
            "],\"tool_choice\":\"auto\",\"parallel_tool_calls\":false,"
            "\"reasoning\":{\"effort\":\"none\",\"exclude\":true},"
            "\"max_tokens\":160,\"temperature\":0.2,\"stream\":") ||
        !body.append(stream ? "true}" : "false}")) {
        body.clear();
        return Error::request_too_large;
    }
    return detail::valid_json_value(body.data(), body.size()) ? Error::none
                                                              : Error::malformed_response;
}

Error build_openrouter_route_request(
    const OpenRouterConfig &config, const ConversationHistory &history,
    const ToolRegistry &tools, const char *approximate_location,
    std::size_t approximate_location_size, const char *current_utc_minute,
    bool stream, ChatRequestBody &body) {
    body.clear();
    if (!valid_config_token(config.chat_model) ||
        !UtcClock::valid_minute_text(current_utc_minute) ||
        !valid_approximate_location(
            approximate_location, approximate_location_size)) {
        return Error::invalid_argument;
    }
    if (!body.append("{\"model\":") ||
        !detail::append_json_string(body, config.chat_model) ||
        !body.append(",\"messages\":[{\"role\":\"system\",\"content\":") ||
        !append_system_message(
            body, routing_prompt, approximate_location,
            approximate_location_size, current_utc_minute) ||
        !body.push_back('}')) {
        return Error::request_too_large;
    }
    for (std::size_t index = 0; index < history.size(); ++index) {
        if (!body.push_back(',') || !append_message(body, history.at(index))) {
            body.clear();
            return Error::request_too_large;
        }
    }
    if (!body.append(
            "],\"tools\":[{\"type\":\"function\",\"function\":{"
            "\"name\":\"answer_direct\","
            "\"description\":\"Use the model without current or visual data.\","
            "\"parameters\":{\"type\":\"object\",\"properties\":{},"
            "\"additionalProperties\":false}}}")) {
        return Error::request_too_large;
    }
    for (std::size_t index = 0; index < tools.size(); ++index) {
        const Tool &tool = tools.at(index);
        if (!body.append(",{\"type\":\"function\",\"function\":{") ||
            !append_key(body, "name") ||
            !detail::append_json_string(body, tool.name()) ||
            !body.push_back(',') || !append_key(body, "description") ||
            !detail::append_json_string(body, tool.description()) ||
            !body.append(",\"parameters\":") ||
            !body.append(tool.parameters_schema()) || !body.append("}}")) {
            body.clear();
            return Error::request_too_large;
        }
    }
    if (!body.append(
            "],\"tool_choice\":\"required\",\"parallel_tool_calls\":false,"
            "\"reasoning\":{\"effort\":\"none\",\"exclude\":true},"
            "\"max_tokens\":96,\"temperature\":0,\"stream\":") ||
        !body.append(stream ? "true}" : "false}")) {
        body.clear();
        return Error::request_too_large;
    }
    return detail::valid_json_value(body.data(), body.size())
               ? Error::none
               : Error::malformed_response;
}

Error build_openrouter_answer_request(
    const OpenRouterConfig &config, const ConversationHistory &history,
    const char *approximate_location, std::size_t approximate_location_size,
    const char *current_utc_minute, bool stream, ChatRequestBody &body) {
    body.clear();
    if (!valid_config_token(config.chat_model) ||
        !UtcClock::valid_minute_text(current_utc_minute) ||
        !valid_approximate_location(
            approximate_location, approximate_location_size)) {
        return Error::invalid_argument;
    }
    if (!body.append("{\"model\":") ||
        !detail::append_json_string(body, config.chat_model) ||
        !body.append(",\"messages\":[{\"role\":\"system\",\"content\":") ||
        !append_system_message(
            body, answer_prompt, approximate_location,
            approximate_location_size, current_utc_minute) ||
        !body.push_back('}')) {
        return Error::request_too_large;
    }
    for (std::size_t index = 0; index < history.size(); ++index) {
        if (!body.push_back(',') || !append_message(body, history.at(index))) {
            body.clear();
            return Error::request_too_large;
        }
    }
    if (!body.append(
            "],\"reasoning\":{\"effort\":\"none\",\"exclude\":true},"
            "\"max_tokens\":160,\"temperature\":0.2,\"stream\":") ||
        !body.append(stream ? "true}" : "false}")) {
        body.clear();
        return Error::request_too_large;
    }
    return detail::valid_json_value(body.data(), body.size())
               ? Error::none
               : Error::malformed_response;
}

Error build_openrouter_transcription_plan(
    const OpenRouterConfig &config, std::size_t audio_file_bytes,
    MultipartTranscriptionPlan &plan) {
    plan = {};
    if (!valid_config_token(config.transcription_model) || audio_file_bytes == 0 ||
        audio_file_bytes > Limits::max_audio_file_bytes) {
        return Error::invalid_argument;
    }
    if (!plan.content_type.append("multipart/form-data; boundary=") ||
        !plan.content_type.append(multipart_boundary) ||
        !plan.preamble.append("--") || !plan.preamble.append(multipart_boundary) ||
        !plan.preamble.append(
            "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n") ||
        !plan.preamble.append(config.transcription_model) ||
        !plan.preamble.append("\r\n--") ||
        !plan.preamble.append(multipart_boundary) ||
        !plan.preamble.append(
            "\r\nContent-Disposition: form-data; name=\"temperature\"\r\n\r\n0"
            "\r\n--") ||
        !plan.preamble.append(multipart_boundary) ||
        !plan.preamble.append(
            "\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\n"
            "json\r\n--") ||
        !plan.preamble.append(multipart_boundary) ||
        !plan.preamble.append(
            "\r\nContent-Disposition: form-data; name=\"file\"; "
            "filename=\"speech.wav\"\r\nContent-Type: audio/wav\r\n\r\n") ||
        !plan.epilogue.append("\r\n--") ||
        !plan.epilogue.append(multipart_boundary) ||
        !plan.epilogue.append("--\r\n")) {
        plan = {};
        return Error::request_too_large;
    }
    if (audio_file_bytes > std::numeric_limits<std::size_t>::max() -
                               plan.preamble.size() - plan.epilogue.size()) {
        plan = {};
        return Error::request_too_large;
    }
    plan.content_length =
        plan.preamble.size() + audio_file_bytes + plan.epilogue.size();
    return Error::none;
}

Error build_openrouter_speech_request(
    const OpenRouterConfig &config, const char *text, std::size_t size,
    SpeechRequestBody &body) {
    body.clear();
    if (!valid_config_token(config.speech_model) ||
        !valid_config_token(config.speech_voice, 64) || text == nullptr ||
        size == 0 || size > Limits::max_tts_input_bytes) {
        return Error::invalid_argument;
    }
    if (!body.append("{\"model\":") ||
        !detail::append_json_string(body, config.speech_model) ||
        !body.append(",\"input\":") ||
        !detail::append_json_string(body, text, size) ||
        !body.append(",\"voice\":") ||
        !detail::append_json_string(body, config.speech_voice) ||
        !body.append(",\"response_format\":\"pcm\"}")) {
        body.clear();
        return Error::request_too_large;
    }
    return Error::none;
}

OpenRouterConfig openrouter_speech_config_for_language(
    const OpenRouterConfig &config, SpeechLanguage language) {
    OpenRouterConfig selected = config;
    if (language == SpeechLanguage::french &&
        config.speech_model != nullptr &&
        std::strcmp(config.speech_model, kokoro_model) == 0) {
        selected.speech_voice = kokoro_french_voice;
    }
    return selected;
}

Error parse_openrouter_chat_response(
    const char *json, std::size_t size, ChatTurn &turn) {
    turn.clear();
    if (json == nullptr || size == 0 || size > Limits::max_chat_response_bytes) {
        return Error::response_too_large;
    }
    ChatTurn raw_turn;
    bool saw_finish = false;
    const Error error =
        parse_chat_json(json, size, raw_turn, false, saw_finish);
    if (error != Error::none || !saw_finish) {
        return error == Error::none ? Error::malformed_response : error;
    }
    if (raw_turn.kind == ChatTurnKind::tool_call) {
        turn = raw_turn;
        return Error::none;
    }
    turn.kind = ChatTurnKind::answer;
    const LanguageTagState tag_state = filter_answer_language(
        raw_turn.answer, turn.answer, turn.speech_language);
    return tag_state == LanguageTagState::ready ? Error::none
                                                : Error::malformed_response;
}

Error parse_openrouter_transcription_response(
    const char *json, std::size_t size,
    FixedText<Limits::max_transcript_bytes> &transcript) {
    transcript.clear();
    if (json == nullptr || size == 0 || size > 8'192) {
        return Error::response_too_large;
    }
    detail::JsonReader reader(json, size);
    if (!reader.consume('{')) return Error::malformed_response;
    bool found = false;
    while (reader.peek() != '}') {
        FixedText<48> key;
        if (!reader.read_string(key) || !reader.consume(':')) {
            return Error::malformed_response;
        }
        if (key.equals("text")) {
            if (found || !reader.read_string(transcript)) {
                return Error::malformed_response;
            }
            found = true;
        } else if (key.equals("error")) {
            if (!reader.skip_value()) return Error::malformed_response;
            return Error::model_failed;
        } else if (!reader.skip_value()) {
            return Error::malformed_response;
        }
        if (reader.consume('}')) break;
        if (!reader.consume(',')) return Error::malformed_response;
    }
    if (!reader.finish() || !found || transcript.empty()) {
        transcript.clear();
        return Error::malformed_response;
    }
    return Error::none;
}

Error validate_openrouter_pcm_content_type(const char *content_type) {
    if (content_type == nullptr) return Error::unsupported_media;
    constexpr char expected[] = "audio/pcm;rate=24000;channels=1";
    return std::strcmp(content_type, expected) == 0 ? Error::none
                                                    : Error::unsupported_media;
}

Error OpenRouterSseParser::feed(const char *data, std::size_t size) {
    if (error_ != Error::none || done_) return error_;
    if (data == nullptr && size != 0) {
        error_ = Error::invalid_argument;
        return error_;
    }
    for (std::size_t index = 0; index < size; ++index) {
        if (data[index] == '\n') {
            error_ = process_line();
            line_.clear();
            if (error_ != Error::none || done_) return error_;
        } else if (!line_.push_back(data[index])) {
            error_ = Error::response_too_large;
            return error_;
        }
    }
    return error_;
}

Error OpenRouterSseParser::process_line() {
    while (!line_.empty() && line_.data()[line_.size() - 1] == '\r') {
        line_.pop_back();
    }
    if (line_.empty() || line_.data()[0] == ':') return Error::none;
    constexpr char prefix[] = "data: ";
    if (line_.size() < sizeof(prefix) - 1 ||
        std::memcmp(line_.data(), prefix, sizeof(prefix) - 1) != 0) {
        return Error::none;
    }
    const char *payload = line_.data() + sizeof(prefix) - 1;
    const std::size_t payload_size = line_.size() - (sizeof(prefix) - 1);
    if (payload_size == 6 && std::memcmp(payload, "[DONE]", 6) == 0) {
        if (!saw_finish_) return Error::malformed_response;
        done_ = true;
        return Error::none;
    }
    const Error parse_error =
        parse_chat_json(payload, payload_size, raw_turn_, true, saw_finish_);
    if (parse_error != Error::none) {
        return parse_error;
    }
    if (raw_turn_.kind == ChatTurnKind::tool_call) {
        turn_ = raw_turn_;
        if (published_size_ != 0 && text_sink_ != nullptr) {
            const Error sink_error = text_sink_->write_chat_text("", 0);
            if (sink_error != Error::none) {
                return sink_error;
            }
        }
        return Error::none;
    }
    turn_.kind = ChatTurnKind::answer;
    const LanguageTagState tag_state = filter_answer_language(
        raw_turn_.answer, turn_.answer, turn_.speech_language);
    if (tag_state == LanguageTagState::pending) {
        return Error::none;
    }
    if (tag_state == LanguageTagState::malformed) {
        return Error::malformed_response;
    }
    language_resolved_ = true;
    if (!language_published_ && text_sink_ != nullptr) {
        const Error sink_error =
            text_sink_->set_speech_language(turn_.speech_language);
        if (sink_error != Error::none) {
            return sink_error;
        }
        language_published_ = true;
    }
    if (turn_.answer.size() <= published_size_) {
        return Error::none;
    }
    published_size_ = turn_.answer.size();
    if (text_sink_ != nullptr) {
        const Error sink_error = text_sink_->write_chat_text(
            turn_.answer.data(), turn_.answer.size());
        if (sink_error != Error::none) {
            return sink_error;
        }
    }
    return Error::none;
}

Error OpenRouterSseParser::finish() {
    if (error_ != Error::none) return error_;
    if (!line_.empty()) {
        error_ = process_line();
        line_.clear();
    }
    if (error_ == Error::none && (!done_ || !saw_finish_)) {
        error_ = Error::malformed_response;
    }
    if (error_ == Error::none && raw_turn_.kind == ChatTurnKind::answer &&
        !language_resolved_) {
        error_ = Error::malformed_response;
    }
    return error_;
}

void OpenRouterSseParser::reset() {
    line_.clear();
    raw_turn_.clear();
    turn_.clear();
    error_ = Error::none;
    published_size_ = 0;
    language_resolved_ = false;
    language_published_ = false;
    saw_finish_ = false;
    done_ = false;
}

}  // namespace agent
}  // namespace chatesp
