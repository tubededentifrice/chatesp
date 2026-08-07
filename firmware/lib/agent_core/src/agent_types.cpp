#include "chatesp/agent_types.hpp"

#include <cstring>
#include <utility>

namespace chatesp {
namespace agent {

Error ConversationHistory::append_text(
    MessageRole role, const char *text, std::size_t length) {
    if ((role != MessageRole::user && role != MessageRole::assistant) ||
        text == nullptr || length == 0 || length > Limits::max_message_bytes) {
        return Error::invalid_argument;
    }
    if (size_ == Limits::max_history_messages) {
        return Error::limit_exceeded;
    }
    Message message;
    message.role = role;
    if (!message.content.assign(text, length)) {
        return Error::limit_exceeded;
    }
    messages_[size_++] = std::move(message);
    return Error::none;
}

Error ConversationHistory::append_tool_call(const ToolInvocation &call) {
    if (call.id.empty() || call.name.empty() || call.arguments.empty()) {
        return Error::invalid_argument;
    }
    if (size_ == Limits::max_history_messages) {
        return Error::limit_exceeded;
    }
    Message message;
    message.role = MessageRole::assistant_tool_call;
    message.tool_call = call;
    messages_[size_++] = std::move(message);
    return Error::none;
}

Error ConversationHistory::append_tool_result(
    const ToolInvocation &call, const char *json, std::size_t length) {
    if (call.id.empty() || call.name.empty() || json == nullptr || length == 0 ||
        length > Limits::max_message_bytes) {
        return Error::invalid_argument;
    }
    if (size_ == Limits::max_history_messages) {
        return Error::limit_exceeded;
    }
    Message message;
    message.role = MessageRole::tool;
    message.tool_call.id = call.id;
    message.tool_call.name = call.name;
    if (!message.content.assign(json, length)) {
        return Error::limit_exceeded;
    }
    messages_[size_++] = std::move(message);
    return Error::none;
}

void ConversationHistory::make_room_for_turn() {
    constexpr std::size_t required = 2 + Limits::max_tool_rounds * 2;
    while (remaining() < required && size_ != 0) {
        discard_oldest_turn();
    }
}

void ConversationHistory::discard_oldest_turn() {
    std::size_t remove_count = 0;
    while (remove_count < size_) {
        const bool completes_turn =
            messages_[remove_count].role == MessageRole::assistant;
        ++remove_count;
        if (completes_turn) {
            break;
        }
    }
    if (remove_count == 0 || remove_count > size_) {
        clear();
        return;
    }
    for (std::size_t index = remove_count; index < size_; ++index) {
        messages_[index - remove_count] = std::move(messages_[index]);
    }
    size_ -= remove_count;
}

void ConversationHistory::clear() {
    for (std::size_t index = 0; index < size_; ++index) {
        messages_[index] = {};
    }
    size_ = 0;
}

void ConversationHistory::truncate(std::size_t size) {
    if (size >= size_) {
        return;
    }
    for (std::size_t index = size; index < size_; ++index) {
        messages_[index] = {};
    }
    size_ = size;
}

bool retry_allowed(
    Error error, std::uint8_t completed_attempts, bool output_started,
    const RequestPolicy &policy) {
    if (output_started || completed_attempts >= policy.max_attempts ||
        policy.max_attempts < 2) {
        return false;
    }
    switch (error) {
        case Error::connect_timeout:
        case Error::first_byte_timeout:
        case Error::disconnected:
        case Error::rate_limited:
        case Error::server_error:
            return true;
        default:
            return false;
    }
}

Error validate_image_fetch_request(const ImageFetchRequest &request) {
    if (request.url == nullptr ||
        std::strncmp(request.url, "https://", 8) != 0) {
        return Error::invalid_argument;
    }
    std::size_t url_size = 0;
    while (url_size <= Limits::max_url_bytes && request.url[url_size] != '\0') {
        ++url_size;
    }
    if (url_size <= 8 || url_size > Limits::max_url_bytes ||
        request.max_bytes == 0 ||
        request.max_bytes > Limits::max_image_download_bytes ||
        request.max_dimension == 0 ||
        request.max_dimension > Limits::max_image_dimension ||
        request.max_redirects > Limits::max_image_redirects ||
        request.policy.max_attempts == 0 || request.policy.max_attempts > 2 ||
        request.policy.connect_timeout_ms > 5'000 ||
        request.policy.first_byte_timeout_ms > 8'000 ||
        request.policy.idle_timeout_ms > 5'000 ||
        request.policy.total_timeout_ms > 20'000) {
        return Error::limit_exceeded;
    }
    return Error::none;
}

}  // namespace agent
}  // namespace chatesp
