#pragma once

#include <cstddef>
#include <cstdint>

#include "chatesp/agent_types.hpp"

namespace chatesp {
namespace agent {

class CancellationToken {
public:
    virtual ~CancellationToken() = default;
    [[nodiscard]] virtual bool cancelled() const = 0;
};

class ByteSink {
public:
    virtual ~ByteSink() = default;
    virtual Error begin(const ImageMetadata &metadata) = 0;
    virtual Error write(const std::uint8_t *data, std::size_t size) = 0;
    virtual Error finish() = 0;
};

class PcmSink {
public:
    virtual ~PcmSink() = default;
    virtual Error begin(
        std::uint32_t sample_rate_hz, std::uint8_t channels,
        std::uint8_t bits_per_sample) = 0;
    virtual Error write(const std::uint8_t *data, std::size_t size) = 0;
    virtual Error finish() = 0;
};

class ChatProvider {
public:
    virtual ~ChatProvider() = default;
    virtual Error complete(
        const ConversationHistory &history, ChatTurn &turn,
        CancellationToken &cancellation) = 0;
};

class TranscriptionProvider {
public:
    virtual ~TranscriptionProvider() = default;
    virtual Error transcribe(
        const AudioView &audio,
        FixedText<Limits::max_transcript_bytes> &transcript,
        CancellationToken &cancellation) = 0;
};

class SpeechProvider {
public:
    virtual ~SpeechProvider() = default;
    virtual Error speak(
        const char *text, std::size_t size, PcmSink &sink,
        CancellationToken &cancellation) = 0;
};

class WebSearchProvider {
public:
    virtual ~WebSearchProvider() = default;
    virtual Error search(
        const char *query, std::size_t size, WebResults &results,
        CancellationToken &cancellation) = 0;
};

class ImageSearchProvider {
public:
    virtual ~ImageSearchProvider() = default;
    virtual Error search(
        const char *query, std::size_t size, ImageResults &results,
        CancellationToken &cancellation) = 0;
};

class ImageFetchProvider {
public:
    virtual ~ImageFetchProvider() = default;
    virtual Error fetch(
        const ImageFetchRequest &request, ByteSink &sink,
        CancellationToken &cancellation) = 0;
};

}  // namespace agent
}  // namespace chatesp
