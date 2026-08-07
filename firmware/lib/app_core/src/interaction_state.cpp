#include "chatesp/interaction_state.hpp"

namespace chatesp {

InteractionStateMachine::InteractionStateMachine(InteractionConfig config)
    : config_(config) {}

void InteractionStateMachine::ready(std::uint32_t now_ms) {
    button_down_ = false;
    wake_press_ = false;
    set_state(InteractionState::idle, now_ms);
    last_activity_at_ms_ = now_ms;
}

void InteractionStateMachine::button_down(std::uint32_t now_ms) {
    if (state_ == InteractionState::sleep_pending || button_down_) {
        return;
    }
    if (state_ != InteractionState::idle) {
        set_state(InteractionState::idle, now_ms);
    }
    button_down_ = true;
    wake_press_ = false;
    button_down_at_ms_ = now_ms;
    last_activity_at_ms_ = now_ms;
}

void InteractionStateMachine::wake_button_down(std::uint32_t now_ms) {
    if (state_ != InteractionState::idle || button_down_) {
        return;
    }
    button_down_ = true;
    wake_press_ = true;
    button_down_at_ms_ = now_ms;
    last_activity_at_ms_ = now_ms;
}

void InteractionStateMachine::button_up(std::uint32_t now_ms) {
    if (!button_down_) {
        return;
    }
    button_down_ = false;
    const bool was_wake_press = wake_press_;
    wake_press_ = false;
    last_activity_at_ms_ = now_ms;
    if (state_ == InteractionState::recording) {
        set_state(InteractionState::transcribing, now_ms);
        return;
    }
    if (state_ == InteractionState::idle) {
        if (!was_wake_press) {
            set_state(InteractionState::sleep_pending, now_ms);
        }
    }
}

void InteractionStateMachine::tick(std::uint32_t now_ms) {
    if (state_ == InteractionState::idle && button_down_ &&
        elapsed(now_ms, button_down_at_ms_) >= config_.talk_hold_ms) {
        set_state(InteractionState::recording, now_ms);
        return;
    }
    if (state_ == InteractionState::idle && !button_down_ &&
        elapsed(now_ms, last_activity_at_ms_) >= config_.idle_sleep_ms) {
        set_state(InteractionState::sleep_pending, now_ms);
        return;
    }
    if (state_ == InteractionState::error &&
        elapsed(now_ms, state_entered_at_ms_) >= config_.error_visible_ms) {
        set_state(InteractionState::idle, now_ms);
        last_activity_at_ms_ = now_ms;
    }
}

void InteractionStateMachine::transcription_ready(std::uint32_t now_ms) {
    if (state_ == InteractionState::transcribing) {
        set_state(InteractionState::thinking, now_ms);
    }
}

void InteractionStateMachine::tool_started(std::uint32_t now_ms) {
    if (state_ == InteractionState::thinking) {
        set_state(InteractionState::tool_work, now_ms);
    }
}

void InteractionStateMachine::speech_started(std::uint32_t now_ms) {
    if (state_ == InteractionState::thinking ||
        state_ == InteractionState::tool_work) {
        set_state(InteractionState::speaking, now_ms);
    }
}

void InteractionStateMachine::interaction_finished(std::uint32_t now_ms) {
    if (state_ == InteractionState::transcribing ||
        state_ == InteractionState::thinking ||
        state_ == InteractionState::tool_work ||
        state_ == InteractionState::speaking) {
        set_state(InteractionState::idle, now_ms);
        last_activity_at_ms_ = now_ms;
    }
}

void InteractionStateMachine::note_idle_activity(std::uint32_t now_ms) {
    if (state_ == InteractionState::idle && !button_down_) {
        last_activity_at_ms_ = now_ms;
    }
}

void InteractionStateMachine::fail(std::uint32_t now_ms) {
    if (state_ != InteractionState::sleep_pending) {
        button_down_ = false;
        wake_press_ = false;
        set_state(InteractionState::error, now_ms);
    }
}

void InteractionStateMachine::cancel_for_sleep() {
    button_down_ = false;
    wake_press_ = false;
    state_ = InteractionState::sleep_pending;
}

InteractionState InteractionStateMachine::state() const { return state_; }

bool InteractionStateMachine::button_is_down() const { return button_down_; }

std::uint32_t InteractionStateMachine::inactivity_ms(
    std::uint32_t now_ms) const {
    return elapsed(now_ms, last_activity_at_ms_);
}

void InteractionStateMachine::set_state(
    InteractionState next, std::uint32_t now_ms) {
    state_ = next;
    state_entered_at_ms_ = now_ms;
}

std::uint32_t InteractionStateMachine::elapsed(
    std::uint32_t now_ms, std::uint32_t since_ms) {
    return now_ms - since_ms;
}

const char *state_name(InteractionState state) {
    switch (state) {
        case InteractionState::booting:
            return "BOOT";
        case InteractionState::idle:
            return "READY";
        case InteractionState::recording:
            return "LISTENING";
        case InteractionState::transcribing:
            return "TRANSCRIBING";
        case InteractionState::thinking:
            return "THINKING";
        case InteractionState::tool_work:
            return "SEARCHING";
        case InteractionState::speaking:
            return "SPEAKING";
        case InteractionState::error:
            return "TRY AGAIN";
        case InteractionState::sleep_pending:
            return "SLEEP";
    }
    return "UNKNOWN";
}

}  // namespace chatesp
