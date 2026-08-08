#pragma once

#include <atomic>
#include <cstdint>

namespace chatesp {
namespace runtime {

enum class ButtonRoute : std::uint8_t { normal, wake };

class AsyncShutdownGate {
public:
    [[nodiscard]] bool begin() {
        State expected = State::idle;
        return state_.compare_exchange_strong(
            expected, State::running, std::memory_order_acq_rel);
    }

    [[nodiscard]] bool cancel_begin() {
        State expected = State::running;
        return state_.compare_exchange_strong(
            expected, State::idle, std::memory_order_acq_rel);
    }

    void complete() {
        state_.store(State::complete, std::memory_order_release);
    }

    [[nodiscard]] bool consume_completion() {
        State expected = State::complete;
        return state_.compare_exchange_strong(
            expected, State::idle, std::memory_order_acq_rel);
    }

    [[nodiscard]] bool running() const {
        return state_.load(std::memory_order_acquire) == State::running;
    }

    [[nodiscard]] bool completed() const {
        return state_.load(std::memory_order_acquire) == State::complete;
    }

private:
    enum class State : std::uint8_t { idle, running, complete };

    std::atomic<State> state_{State::idle};
};

class MonotonicDeadline {
public:
    MonotonicDeadline(std::uint32_t started_ms, std::uint32_t timeout_ms)
        : started_ms_(started_ms), timeout_ms_(timeout_ms) {}

    [[nodiscard]] bool expired(std::uint32_t now_ms) const {
        return now_ms - started_ms_ >= timeout_ms_;
    }

private:
    std::uint32_t started_ms_ = 0;
    std::uint32_t timeout_ms_ = 0;
};

class PoweroffGate {
public:
    void begin_sleep() {
        state_.store(State::preparing, std::memory_order_release);
    }

    [[nodiscard]] bool mark_soft_sleep() {
        State expected = State::preparing;
        return state_.compare_exchange_strong(
            expected, State::soft_sleep, std::memory_order_acq_rel);
    }

    [[nodiscard]] bool mark_poweroff_ready() {
        State expected = State::preparing;
        return state_.compare_exchange_strong(
            expected, State::poweroff_ready, std::memory_order_acq_rel);
    }

    [[nodiscard]] ButtonRoute button_down() {
        const State prior =
            state_.exchange(State::awake, std::memory_order_acq_rel);
        return prior == State::awake ? ButtonRoute::normal
                                     : ButtonRoute::wake;
    }

    void recover() {
        state_.store(State::awake, std::memory_order_release);
    }

    [[nodiscard]] bool poweroff_ready() const {
        return state_.load(std::memory_order_acquire) ==
            State::poweroff_ready;
    }

private:
    enum class State : std::uint8_t {
        awake,
        preparing,
        soft_sleep,
        poweroff_ready,
    };

    std::atomic<State> state_{State::awake};
};

}  // namespace runtime
}  // namespace chatesp
