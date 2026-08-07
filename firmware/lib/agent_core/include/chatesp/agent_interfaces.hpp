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
    virtual void abort() = 0;
};

class PcmSink {
public:
    virtual ~PcmSink() = default;
    virtual Error begin(
        std::uint32_t sample_rate_hz, std::uint8_t channels,
        std::uint8_t bits_per_sample) = 0;
    virtual Error write(const std::uint8_t *data, std::size_t size) = 0;
    virtual Error finish() = 0;
    virtual void cancel() {}
};

class ChatTextSink {
public:
    virtual ~ChatTextSink() = default;

    // Text is private user content. The sink must not log or retain it after
    // the active request. Each update is the complete answer received so far.
    virtual Error write_chat_text(const char *text, std::size_t size) = 0;
};

class NullChatTextSink final : public ChatTextSink {
public:
    Error write_chat_text(const char *, std::size_t) override {
        return Error::none;
    }
};

class ChatProvider {
public:
    virtual ~ChatProvider() = default;
    virtual Error complete(
        const ConversationHistory &history, ChatTurn &turn,
        CancellationToken &cancellation) = 0;

    // A provider without a streaming implementation uses this safe fallback.
    // It publishes only a complete answer and never publishes a tool call.
    virtual Error complete_streaming(
        const ConversationHistory &history, ChatTurn &turn,
        ChatTextSink &text_sink, CancellationToken &cancellation) {
        const Error error = complete(history, turn, cancellation);
        if (error != Error::none || cancellation.cancelled() ||
            turn.kind != ChatTurnKind::answer || turn.answer.empty()) {
            return cancellation.cancelled() ? Error::cancelled : error;
        }
        return text_sink.write_chat_text(
            turn.answer.data(), turn.answer.size());
    }
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
