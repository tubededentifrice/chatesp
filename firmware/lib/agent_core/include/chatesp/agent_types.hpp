#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "chatesp/agent_limits.hpp"
#include "chatesp/fixed_text.hpp"

namespace chatesp {
namespace agent {

enum class Error : std::uint8_t {
    none,
    invalid_argument,
    limit_exceeded,
    malformed_response,
    cancelled,
    connect_timeout,
    first_byte_timeout,
    idle_timeout,
    total_timeout,
    disconnected,
    rate_limited,
    authentication,
    payment_required,
    server_error,
    unsupported_media,
    tool_not_found,
    tool_failed,
    model_failed,
};

enum class MessageRole : std::uint8_t {
    user,
    assistant,
    assistant_tool_call,
    tool,
};

struct ToolInvocation {
    FixedText<Limits::max_tool_call_id_bytes> id;
    FixedText<Limits::max_tool_name_bytes> name;
    FixedText<Limits::max_tool_arguments_bytes> arguments;
};

enum class PowerOffMode : std::uint8_t {
    system_off,
    development_sleep,
};

struct DeviceStatus {
    std::uint8_t brightness_percent = 65;
    std::uint8_t volume_percent = 70;
    std::uint8_t battery_percent = 0;
    bool battery_available = false;
    bool settings_persistent = false;
    PowerOffMode power_off_mode = PowerOffMode::system_off;
};

struct Message {
    MessageRole role = MessageRole::user;
    FixedText<Limits::max_message_bytes> content;
    ToolInvocation tool_call;
};

class ConversationHistory {
public:
    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] std::size_t remaining() const {
        return Limits::max_history_messages - size_;
    }
    [[nodiscard]] const Message &at(std::size_t index) const {
        return messages_[index];
    }

    Error append_text(MessageRole role, const char *text, std::size_t length);
    Error append_tool_call(const ToolInvocation &call);
    Error append_tool_result(
        const ToolInvocation &call, const char *json, std::size_t length);
    void make_room_for_turn();
    void truncate(std::size_t size);
    void clear();

private:
    void discard_oldest_turn();

    std::array<Message, Limits::max_history_messages> messages_{};
    std::size_t size_ = 0;
};

enum class ChatTurnKind : std::uint8_t { answer, tool_call };

struct ChatTurn {
    ChatTurnKind kind = ChatTurnKind::answer;
    FixedText<Limits::max_answer_bytes> answer;
    ToolInvocation tool_call;

    void clear() {
        kind = ChatTurnKind::answer;
        answer.clear();
        tool_call.id.clear();
        tool_call.name.clear();
        tool_call.arguments.clear();
    }
};

enum class TurnRouteKind : std::uint8_t {
    direct_answer,
    web_search,
    image_search,
    tool_call,
};

struct TurnRoute {
    TurnRouteKind kind = TurnRouteKind::direct_answer;
    ToolInvocation tool_call;

    void clear() {
        kind = TurnRouteKind::direct_answer;
        tool_call.id.clear();
        tool_call.name.clear();
        tool_call.arguments.clear();
    }
};

struct WebResult {
    FixedText<Limits::max_title_bytes> title;
    FixedText<Limits::max_url_bytes> url;
    FixedText<Limits::max_snippet_bytes> snippet;

    void clear() {
        title.clear();
        url.clear();
        snippet.clear();
    }
};

struct WebResults {
    std::array<WebResult, Limits::max_web_results> items{};
    std::size_t size = 0;

    void clear() {
        for (auto &item : items) {
            item.clear();
        }
        size = 0;
    }
};

struct ImageResult {
    FixedText<Limits::max_image_id_bytes> id;
    FixedText<Limits::max_title_bytes> title;
    FixedText<Limits::max_url_bytes> page_url;
    FixedText<Limits::max_url_bytes> thumbnail_url;
    FixedText<Limits::max_url_bytes> image_url;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    void clear() {
        id.clear();
        title.clear();
        page_url.clear();
        thumbnail_url.clear();
        image_url.clear();
        width = 0;
        height = 0;
    }
};

struct ImageResults {
    std::array<ImageResult, Limits::max_image_results> items{};
    std::size_t size = 0;

    void clear() {
        for (auto &item : items) {
            item.clear();
        }
        size = 0;
    }
};

struct AudioView {
    const std::uint8_t *data = nullptr;
    std::size_t size = 0;
    std::uint32_t sample_rate_hz = 16'000;
    std::uint8_t channels = 1;
    std::uint8_t bits_per_sample = 16;
};

struct ImageFetchRequest {
    const char *url = nullptr;
    std::size_t max_bytes = Limits::max_image_download_bytes;
    std::uint32_t max_dimension = Limits::max_image_dimension;
    std::uint8_t max_redirects = Limits::max_image_redirects;
    RequestPolicy policy = image_fetch_policy();
};

enum class ImageMediaType : std::uint8_t { jpeg, png, webp };

struct ImageMetadata {
    ImageMediaType media_type = ImageMediaType::jpeg;
    std::size_t content_length = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

[[nodiscard]] bool retry_allowed(
    Error error, std::uint8_t completed_attempts, bool output_started,
    const RequestPolicy &policy);

[[nodiscard]] Error validate_image_fetch_request(
    const ImageFetchRequest &request);

}  // namespace agent
}  // namespace chatesp
