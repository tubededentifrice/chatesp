#include "chatesp/agent_loop.hpp"

namespace chatesp {
namespace agent {

Error AgentLoop::run(
    const char *user_text, std::size_t size,
    FixedText<Limits::max_answer_bytes> &answer,
    CancellationToken &cancellation) {
    answer.clear();
    route_.clear();
    turn_.clear();
    tool_result_.clear();
    answer_pending_speech_.store(false, std::memory_order_release);
    if (user_text == nullptr || size == 0 ||
        size > Limits::max_transcript_bytes) {
        return Error::invalid_argument;
    }
    if (cancellation.cancelled()) {
        return Error::cancelled;
    }
    notify(AgentProgressEvent::transcription_complete);
    if (cancellation.cancelled()) {
        return Error::cancelled;
    }

    history_.make_room_for_turn();
    const std::size_t checkpoint = history_.size();
    const auto fail = [this, checkpoint](Error failure) {
        route_.clear();
        turn_.clear();
        tool_result_.clear();
        history_.truncate(checkpoint);
        return failure;
    };
    Error error = history_.append_text(MessageRole::user, user_text, size);
    if (error != Error::none) {
        return fail(error);
    }

    bool route_complete = false;
    for (std::size_t round = 0; round <= Limits::max_tool_rounds; ++round) {
        if (cancellation.cancelled()) {
            return fail(Error::cancelled);
        }
        notify(AgentProgressEvent::model_start);
        if (cancellation.cancelled()) {
            return fail(Error::cancelled);
        }
        route_.clear();
        error = chat_.route_turn(history_, route_, cancellation);
        if (error != Error::none) {
            return fail(error);
        }
        if (cancellation.cancelled()) {
            return fail(Error::cancelled);
        }
        if (route_.kind == TurnRouteKind::direct_answer) {
            route_complete = true;
            break;
        }
        if (round == Limits::max_tool_rounds) {
            return fail(Error::tool_round_limit);
        }

        error = history_.append_tool_call(route_.tool_call);
        if (error != Error::none) {
            return fail(error);
        }
        tool_result_.clear();
        notify(AgentProgressEvent::tool_start);
        if (cancellation.cancelled()) {
            return fail(Error::cancelled);
        }
        error = tools_.execute(
            route_.tool_call, tool_result_, cancellation);
        if (error == Error::cancelled || cancellation.cancelled()) {
            return fail(Error::cancelled);
        }
        if (error != Error::none &&
            !tool_result_.assign(
                "{\"error\":\"requested_data_temporarily_unavailable\"}")) {
            return fail(Error::limit_exceeded);
        }
        error = history_.append_tool_result(
            route_.tool_call, tool_result_.data(), tool_result_.size());
        if (error != Error::none) {
            return fail(error);
        }
        if (tools_.ends_tool_sequence(route_.tool_call)) {
            route_complete = true;
            break;
        }
    }
    if (!route_complete) {
        return fail(Error::tool_round_limit);
    }

    if (cancellation.cancelled()) {
        return fail(Error::cancelled);
    }
    notify(AgentProgressEvent::answer_start);
    notify(AgentProgressEvent::model_start);
    if (cancellation.cancelled()) {
        return fail(Error::cancelled);
    }
    NullChatTextSink null_text_sink;
    ChatTextSink &text_sink =
        text_sink_ == nullptr ? static_cast<ChatTextSink &>(null_text_sink)
                              : *text_sink_;
    turn_.clear();
    answer_pending_speech_.store(true, std::memory_order_release);
    error = chat_.complete_answer_streaming(
        history_, turn_, text_sink, cancellation);
    if (error != Error::none || cancellation.cancelled()) {
        answer_pending_speech_.store(false, std::memory_order_release);
        return fail(cancellation.cancelled() ? Error::cancelled : error);
    }
    if (turn_.kind != ChatTurnKind::answer || turn_.answer.empty()) {
        answer_pending_speech_.store(false, std::memory_order_release);
        return fail(Error::malformed_response);
    }
    error = history_.append_text(
        MessageRole::assistant, turn_.answer.data(), turn_.answer.size());
    if (error != Error::none) {
        answer_pending_speech_.store(false, std::memory_order_release);
        return fail(error);
    }
    notify(AgentProgressEvent::answer_ready);
    if (cancellation.cancelled()) {
        answer_pending_speech_.store(false, std::memory_order_release);
        return fail(Error::cancelled);
    }
    answer = turn_.answer;
    route_.clear();
    turn_.clear();
    tool_result_.clear();
    return Error::none;
}

void AgentLoop::report_speech_start() {
    if (!answer_pending_speech_.exchange(
            false, std::memory_order_acq_rel)) {
        return;
    }
    notify(AgentProgressEvent::speech_start);
}

void AgentLoop::notify(AgentProgressEvent event) {
    if (observer_ != nullptr) {
        observer_->on_agent_progress(event);
    }
}

}  // namespace agent
}  // namespace chatesp
