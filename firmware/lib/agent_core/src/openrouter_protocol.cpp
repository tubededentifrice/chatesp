#include "chatesp/openrouter_protocol.hpp"

#include <cstring>
#include <limits>

#include "chatesp/system_prompt.hpp"
#include "json.hpp"

namespace chatesp {
namespace agent {
namespace {

constexpr char multipart_boundary[] = "ChatESPBoundary7MA4YWxk";

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
    const ToolRegistry &tools, bool stream, ChatRequestBody &body) {
    body.clear();
    if (!valid_config_token(config.chat_model)) return Error::invalid_argument;
    if (!body.append("{\"model\":") ||
        !detail::append_json_string(body, config.chat_model) ||
        !body.append(",\"messages\":[{\"role\":\"system\",\"content\":") ||
        !detail::append_json_string(body, system_prompt) || !body.push_back('}')) {
        return Error::limit_exceeded;
    }
    for (std::size_t index = 0; index < history.size(); ++index) {
        if (!body.push_back(',') || !append_message(body, history.at(index))) {
            body.clear();
            return Error::limit_exceeded;
        }
    }
    if (!body.append("],\"tools\":[")) return Error::limit_exceeded;
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
            return Error::limit_exceeded;
        }
    }
    if (!body.append(
            "],\"tool_choice\":\"auto\",\"parallel_tool_calls\":false,"
            "\"reasoning\":{\"effort\":\"none\",\"exclude\":true},"
            "\"max_tokens\":160,\"temperature\":0.2,\"stream\":") ||
        !body.append(stream ? "true}" : "false}")) {
        body.clear();
        return Error::limit_exceeded;
    }
    return detail::valid_json_value(body.data(), body.size()) ? Error::none
                                                              : Error::malformed_response;
}

Error build_openrouter_route_request(
    const OpenRouterConfig &config, const ConversationHistory &history,
    const ToolRegistry &tools, bool stream, ChatRequestBody &body) {
    body.clear();
    if (!valid_config_token(config.chat_model)) return Error::invalid_argument;
    if (!body.append("{\"model\":") ||
        !detail::append_json_string(body, config.chat_model) ||
        !body.append(",\"messages\":[{\"role\":\"system\",\"content\":") ||
        !detail::append_json_string(body, routing_prompt) ||
        !body.push_back('}')) {
        return Error::limit_exceeded;
    }
    for (std::size_t index = 0; index < history.size(); ++index) {
        if (!body.push_back(',') || !append_message(body, history.at(index))) {
            body.clear();
            return Error::limit_exceeded;
        }
    }
    if (!body.append(
            "],\"tools\":[{\"type\":\"function\",\"function\":{"
            "\"name\":\"answer_direct\","
            "\"description\":\"Use the model without current or visual data.\","
            "\"parameters\":{\"type\":\"object\",\"properties\":{},"
            "\"additionalProperties\":false}}}")) {
        return Error::limit_exceeded;
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
            return Error::limit_exceeded;
        }
    }
    if (!body.append(
            "],\"tool_choice\":\"required\",\"parallel_tool_calls\":false,"
            "\"reasoning\":{\"effort\":\"none\",\"exclude\":true},"
            "\"max_tokens\":96,\"temperature\":0,\"stream\":") ||
        !body.append(stream ? "true}" : "false}")) {
        body.clear();
        return Error::limit_exceeded;
    }
    return detail::valid_json_value(body.data(), body.size())
               ? Error::none
               : Error::malformed_response;
}

Error build_openrouter_answer_request(
    const OpenRouterConfig &config, const ConversationHistory &history,
    bool stream, ChatRequestBody &body) {
    body.clear();
    if (!valid_config_token(config.chat_model)) return Error::invalid_argument;
    if (!body.append("{\"model\":") ||
        !detail::append_json_string(body, config.chat_model) ||
        !body.append(",\"messages\":[{\"role\":\"system\",\"content\":") ||
        !detail::append_json_string(body, answer_prompt) ||
        !body.push_back('}')) {
        return Error::limit_exceeded;
    }
    for (std::size_t index = 0; index < history.size(); ++index) {
        if (!body.push_back(',') || !append_message(body, history.at(index))) {
            body.clear();
            return Error::limit_exceeded;
        }
    }
    if (!body.append(
            "],\"reasoning\":{\"effort\":\"none\",\"exclude\":true},"
            "\"max_tokens\":160,\"temperature\":0.2,\"stream\":") ||
        !body.append(stream ? "true}" : "false}")) {
        body.clear();
        return Error::limit_exceeded;
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
        return Error::limit_exceeded;
    }
    if (audio_file_bytes > std::numeric_limits<std::size_t>::max() -
                               plan.preamble.size() - plan.epilogue.size()) {
        plan = {};
        return Error::limit_exceeded;
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
        return Error::limit_exceeded;
    }
    return Error::none;
}

Error parse_openrouter_chat_response(
    const char *json, std::size_t size, ChatTurn &turn) {
    turn.clear();
    if (json == nullptr || size == 0 || size > Limits::max_chat_response_bytes) {
        return Error::limit_exceeded;
    }
    bool saw_finish = false;
    const Error error = parse_chat_json(json, size, turn, false, saw_finish);
    return error == Error::none && saw_finish ? Error::none
                                              : (error == Error::none
                                                     ? Error::malformed_response
                                                     : error);
}

Error parse_openrouter_transcription_response(
    const char *json, std::size_t size,
    FixedText<Limits::max_transcript_bytes> &transcript) {
    transcript.clear();
    if (json == nullptr || size == 0 || size > 8'192) {
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
            error_ = Error::limit_exceeded;
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
        parse_chat_json(payload, payload_size, turn_, true, saw_finish_);
    if (parse_error != Error::none) {
        return parse_error;
    }
    if (turn_.kind == ChatTurnKind::tool_call) {
        if (published_size_ != 0 && text_sink_ != nullptr) {
            const Error sink_error = text_sink_->write_chat_text("", 0);
            if (sink_error != Error::none) {
                return sink_error;
            }
        }
        return Error::none;
    }
    if (
        turn_.answer.size() <= published_size_) {
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
    return error_;
}

void OpenRouterSseParser::reset() {
    line_.clear();
    turn_.clear();
    error_ = Error::none;
    published_size_ = 0;
    saw_finish_ = false;
    done_ = false;
}

}  // namespace agent
}  // namespace chatesp
