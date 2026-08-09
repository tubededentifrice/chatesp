#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "chatesp/speech_segmenter.hpp"

namespace chatesp {
namespace runtime {

enum class SpeechQueueResult : std::uint8_t {
    ready,
    empty,
    full,
    finished,
    cancelled,
    invalid,
};

// This class owns and wipes two bounded text slots. The caller serializes
// access when a producer and a consumer use it on different tasks.
class SpeechSegmentQueue final : public SpeechSegmentSink {
public:
    static constexpr std::size_t kCapacity =
        SpeechSegmenter::kMaximumSegments;
    static constexpr std::size_t kSegmentBytes =
        SpeechSegmenter::kMaximumSegmentBytes;

    ~SpeechSegmentQueue() override;

    bool push_speech_segment(const char *text, std::size_t size) override;
    SpeechQueueResult pop(char *output, std::size_t capacity,
                          std::size_t &size);
    void finish();
    void discard_pending_and_finish();
    void cancel();
    void reset();

    [[nodiscard]] bool full() const { return count_ == kCapacity; }
    [[nodiscard]] bool empty() const { return count_ == 0; }
    [[nodiscard]] bool finished() const { return finished_; }
    [[nodiscard]] bool cancelled() const { return cancelled_; }

private:
    struct Slot {
        std::array<char, kSegmentBytes + 1> text{};
        std::size_t size = 0;
    };

    static void wipe(Slot &slot);

    std::array<Slot, kCapacity> slots_{};
    std::size_t read_at_ = 0;
    std::size_t write_at_ = 0;
    std::size_t count_ = 0;
    bool finished_ = false;
    bool cancelled_ = false;
};

}  // namespace runtime
}  // namespace chatesp
