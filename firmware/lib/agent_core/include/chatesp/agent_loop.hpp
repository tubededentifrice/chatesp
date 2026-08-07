#pragma once

#include <cstddef>

#include "chatesp/tool_registry.hpp"

namespace chatesp {
namespace agent {

class AgentLoop {
public:
    AgentLoop(ChatProvider &chat, const ToolRegistry &tools)
        : chat_(chat), tools_(tools) {}

    Error run(
        const char *user_text, std::size_t size,
        FixedText<Limits::max_answer_bytes> &answer,
        CancellationToken &cancellation);
    void clear_thread() { history_.clear(); }
    [[nodiscard]] const ConversationHistory &history() const {
        return history_;
    }

private:
    ChatProvider &chat_;
    const ToolRegistry &tools_;
    ConversationHistory history_;
};

}  // namespace agent
}  // namespace chatesp
