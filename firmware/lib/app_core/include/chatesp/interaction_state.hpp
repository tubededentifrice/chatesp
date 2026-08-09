#pragma once

#include <cstdint>

namespace chatesp {

enum class InteractionState : std::uint8_t {
    booting,
    idle,
    recording,
    transcribing,
    thinking,
    tool_work,
    speaking,
    error,
    sleep_pending,
};

struct InteractionConfig {
    std::uint32_t talk_hold_ms = 350;
    std::uint32_t idle_sleep_ms = 30'000;
    std::uint32_t error_visible_ms = 2'200;
    std::uint32_t sleep_press_min_ms = 80;
};

class InteractionStateMachine {
public:
    explicit InteractionStateMachine(InteractionConfig config = {});

    void ready(std::uint32_t now_ms);
    void button_down(std::uint32_t now_ms);
    void wake_button_down(std::uint32_t now_ms);
    void button_up(
        std::uint32_t now_ms, bool short_press_confirmed = true);
    void tick(std::uint32_t now_ms);
    void transcription_ready(std::uint32_t now_ms);
    void tool_started(std::uint32_t now_ms);
    void speech_started(std::uint32_t now_ms);
    void interaction_finished(std::uint32_t now_ms);
    void note_idle_activity(std::uint32_t now_ms);
    void fail(std::uint32_t now_ms);
    void cancel_for_sleep();

    [[nodiscard]] InteractionState state() const;
    [[nodiscard]] bool button_is_down() const;
    [[nodiscard]] std::uint32_t inactivity_ms(std::uint32_t now_ms) const;

private:
    void set_state(InteractionState next, std::uint32_t now_ms);
    [[nodiscard]] static std::uint32_t elapsed(
        std::uint32_t now_ms, std::uint32_t since_ms);

    InteractionConfig config_;
    InteractionState state_ = InteractionState::booting;
    std::uint32_t button_down_at_ms_ = 0;
    std::uint32_t last_activity_at_ms_ = 0;
    std::uint32_t state_entered_at_ms_ = 0;
    bool button_down_ = false;
    bool wake_press_ = false;
};

const char *state_name(InteractionState state);

}  // namespace chatesp
