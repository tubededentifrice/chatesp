#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>

#include "chatesp/tool_registry.hpp"

namespace chatesp {
namespace agent {

enum class AgentProgressEvent : std::uint8_t {
    transcription_complete,
    model_start,
    tool_start,
    answer_start,
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
        AgentProgressObserver *observer = nullptr,
        ChatTextSink *text_sink = nullptr)
        : chat_(chat), tools_(tools), observer_(observer),
          text_sink_(text_sink) {}

    Error run(
        const char *user_text, std::size_t size,
        FixedText<Limits::max_answer_bytes> &answer,
        CancellationToken &cancellation);
    void report_speech_start();
    void clear_thread() {
        history_.clear();
        route_.clear();
        turn_.clear();
        tool_result_.clear();
        answer_pending_speech_.store(false, std::memory_order_release);
    }
    [[nodiscard]] const ConversationHistory &history() const {
        return history_;
    }

private:
    void notify(AgentProgressEvent event);

    ChatProvider &chat_;
    const ToolRegistry &tools_;
    AgentProgressObserver *observer_ = nullptr;
    ChatTextSink *text_sink_ = nullptr;
    ConversationHistory history_;
    TurnRoute route_;
    ChatTurn turn_;
    FixedText<Limits::max_tool_result_bytes> tool_result_;
    std::atomic<bool> answer_pending_speech_{false};
};

}  // namespace agent
}  // namespace chatesp
