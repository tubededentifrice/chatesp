#pragma once

#include <atomic>
#include <cstdint>

namespace chatesp {

enum class AudioSession : std::uint8_t {
    none,
    capture,
    playback,
};

class AudioSessionGate {
public:
    [[nodiscard]] bool try_acquire(AudioSession session) {
        if (session == AudioSession::none) {
            return false;
        }
        AudioSession expected = AudioSession::none;
        return session_.compare_exchange_strong(
            expected, session, std::memory_order_acq_rel);
    }

    void release(AudioSession session) {
        AudioSession expected = session;
        session_.compare_exchange_strong(
            expected, AudioSession::none, std::memory_order_acq_rel);
    }

    [[nodiscard]] AudioSession current() const {
        return session_.load(std::memory_order_acquire);
    }

private:
    std::atomic<AudioSession> session_{AudioSession::none};
};

inline AudioSessionGate &shared_audio_session_gate() {
    static AudioSessionGate gate;
    return gate;
}

}  // namespace chatesp
