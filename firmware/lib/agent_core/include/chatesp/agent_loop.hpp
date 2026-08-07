#pragma once

#include <cstddef>
#include <cstdint>

#include "chatesp/tool_registry.hpp"

namespace chatesp {
namespace agent {

enum class AgentProgressEvent : std::uint8_t {
    transcription_complete,
    model_start,
    tool_start,
    answer_ready,
    speech_start,
};

class AgentProgressObserver {
public:
    virtual ~AgentProgressObserver() = default;

    // This callback must return promptly. Events contain no private content.
    virtual void on_agent_progress(AgentProgressEvent event) = 0;
};

class AgentLoop {
public:
    AgentLoop(
        ChatProvider &chat, const ToolRegistry &tools,
        AgentProgressObserver *observer = nullptr)
        : chat_(chat), tools_(tools), observer_(observer) {}

    Error run(
        const char *user_text, std::size_t size,
        FixedText<Limits::max_answer_bytes> &answer,
        CancellationToken &cancellation);
    void report_speech_start();
    void clear_thread() {
        history_.clear();
        answer_pending_speech_ = false;
    }
    [[nodiscard]] const ConversationHistory &history() const {
        return history_;
    }

private:
    void notify(AgentProgressEvent event);

    ChatProvider &chat_;
    const ToolRegistry &tools_;
    AgentProgressObserver *observer_ = nullptr;
    ConversationHistory history_;
    bool answer_pending_speech_ = false;
};

}  // namespace agent
}  // namespace chatesp
