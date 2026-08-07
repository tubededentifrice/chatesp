#include "chatesp/agent_loop.hpp"

namespace chatesp {
namespace agent {

Error AgentLoop::run(
    const char *user_text, std::size_t size,
    FixedText<Limits::max_answer_bytes> &answer,
    CancellationToken &cancellation) {
    answer.clear();
    if (user_text == nullptr || size == 0 ||
        size > Limits::max_transcript_bytes) {
        return Error::invalid_argument;
    }
    if (cancellation.cancelled()) {
        return Error::cancelled;
    }

    history_.make_room_for_turn();
    const std::size_t checkpoint = history_.size();
    const auto fail = [this, checkpoint](Error failure) {
        history_.truncate(checkpoint);
        return failure;
    };
    Error error = history_.append_text(MessageRole::user, user_text, size);
    if (error != Error::none) {
        return fail(error);
    }

    for (std::size_t round = 0; round <= Limits::max_tool_rounds; ++round) {
        if (cancellation.cancelled()) {
            return fail(Error::cancelled);
        }
        ChatTurn turn;
        error = chat_.complete(history_, turn, cancellation);
        if (error != Error::none) {
            return fail(error);
        }
        if (cancellation.cancelled()) {
            return fail(Error::cancelled);
        }
        if (turn.kind == ChatTurnKind::answer) {
            if (turn.answer.empty()) {
                return fail(Error::malformed_response);
            }
            error = history_.append_text(
                MessageRole::assistant, turn.answer.data(), turn.answer.size());
            if (error != Error::none) {
                return fail(error);
            }
            answer = turn.answer;
            return Error::none;
        }
        if (round == Limits::max_tool_rounds) {
            return fail(Error::limit_exceeded);
        }

        error = history_.append_tool_call(turn.tool_call);
        if (error != Error::none) {
            return fail(error);
        }
        FixedText<Limits::max_tool_result_bytes> result;
        error = tools_.execute(turn.tool_call, result, cancellation);
        if (error == Error::cancelled || cancellation.cancelled()) {
            return fail(Error::cancelled);
        }
        if (error != Error::none &&
            !result.assign("{\"error\":\"Tool is unavailable.\"}")) {
            return fail(Error::limit_exceeded);
        }
        error = history_.append_tool_result(
            turn.tool_call, result.data(), result.size());
        if (error != Error::none) {
            return fail(error);
        }
    }
    return fail(Error::limit_exceeded);
}

}  // namespace agent
}  // namespace chatesp
