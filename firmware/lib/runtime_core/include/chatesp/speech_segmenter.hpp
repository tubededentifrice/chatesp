#pragma once

#include <array>
#include <cstddef>

namespace chatesp {
namespace runtime {

class SpeechSegmentSink {
public:
    virtual ~SpeechSegmentSink() = default;
    virtual bool push_speech_segment(const char *text, std::size_t size) = 0;
};

class SpeechSegmenter {
public:
    static constexpr std::size_t kFirstRequestBytes = 160;
    static constexpr std::size_t kMaximumSegmentBytes = 640;
    static constexpr std::size_t kMaximumSegments = 2;
    static constexpr std::size_t kMaximumSpeechBytes = 640;

    ~SpeechSegmenter();

    // Each update is the complete answer received so far.
    bool update(
        const char *text, std::size_t size, SpeechSegmentSink &sink);
    bool finish(SpeechSegmentSink &sink);
    void reset();

    [[nodiscard]] std::size_t emitted_bytes() const {
        return emitted_bytes_;
    }
    [[nodiscard]] std::size_t emitted_segments() const {
        return emitted_segments_;
    }

private:
    bool consume(char value, SpeechSegmentSink &sink);
    bool emit_first_request(SpeechSegmentSink &sink);
    bool emit_prefix(std::size_t size, SpeechSegmentSink &sink);
    void trim_incomplete_code_point(char excluded_value);
    void discard_pending();
    [[nodiscard]] bool capped() const;

    std::array<char, kMaximumSpeechBytes + 1> pending_{};
    std::size_t pending_size_ = 0;
    std::size_t published_size_ = 0;
    std::size_t emitted_bytes_ = 0;
    std::size_t emitted_segments_ = 0;
};

}  // namespace runtime
}  // namespace chatesp
