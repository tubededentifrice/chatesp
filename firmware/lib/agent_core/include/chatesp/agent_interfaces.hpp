#pragma once

#include <cstddef>
#include <cstdint>

#include "chatesp/agent_types.hpp"
#include "chatesp/memory.hpp"

namespace chatesp {
namespace agent {

class CancellationToken {
public:
    virtual ~CancellationToken() = default;
    [[nodiscard]] virtual bool cancelled() const = 0;
};

class DeviceControlProvider {
public:
    virtual ~DeviceControlProvider() = default;
    virtual Error status(DeviceStatus &status) = 0;
    virtual Error set_brightness(
        std::uint8_t percent, bool &persisted) = 0;
    virtual Error set_volume(
        std::uint8_t percent, bool &persisted) = 0;
    virtual Error schedule_power_off(PowerOffMode &mode) = 0;
    virtual Error schedule_restart() = 0;
};

class MemoryControlProvider {
public:
    virtual ~MemoryControlProvider() = default;
    virtual Error snapshot(MemorySnapshot &snapshot) = 0;
    virtual Error remember(
        const char *fact, std::size_t size, MemoryMutationResult &result) = 0;
    virtual Error forget(
        std::uint32_t id, MemoryMutationResult &result) = 0;
    virtual Error clear_memories(MemoryMutationResult &result) = 0;
    virtual Error compact(
        const MemoryCompactionPlan &plan, MemoryMutationResult &result) = 0;
    virtual void clear_turn_state() = 0;
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

    // A streaming provider reports the language before it publishes text.
    virtual Error set_speech_language(SpeechLanguage) {
        return Error::none;
    }

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
        const Error language_error =
            text_sink.set_speech_language(turn.speech_language);
        if (language_error != Error::none) {
            return language_error;
        }
        return text_sink.write_chat_text(
            turn.answer.data(), turn.answer.size());
    }

    // Routing is complete before final answer text can reach a speech sink.
    // Providers can override this method with a short required-tool request.
    virtual Error route_turn(
        const ConversationHistory &history, TurnRoute &route,
        CancellationToken &cancellation) {
        ChatTurn turn;
        const Error error = complete(history, turn, cancellation);
        if (error != Error::none || cancellation.cancelled()) {
            return cancellation.cancelled() ? Error::cancelled : error;
        }
        route.clear();
        if (turn.kind == ChatTurnKind::tool_call) {
            route.kind = TurnRouteKind::tool_call;
            route.tool_call = turn.tool_call;
        }
        return Error::none;
    }

    // This request must not permit tool calls. Every published byte is final
    // answer text and is safe to send to the sentence speech pipeline.
    virtual Error complete_answer_streaming(
        const ConversationHistory &history, ChatTurn &turn,
        ChatTextSink &text_sink, CancellationToken &cancellation) {
        return complete_streaming(history, turn, text_sink, cancellation);
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
    virtual void set_language(SpeechLanguage) {}
    virtual Error speak(
        const char *text, std::size_t size, PcmSink &sink,
        CancellationToken &cancellation) = 0;

    class SegmentSource {
    public:
        virtual ~SegmentSource() = default;
        virtual Error next(
            FixedText<Limits::max_tts_segment_bytes> &segment, bool &done,
            CancellationToken &cancellation) = 0;
    };

    // The production provider keeps one playback session open for all
    // segments. This fallback keeps test providers source-compatible.
    virtual Error speak_segments(
        SegmentSource &source, PcmSink &sink,
        CancellationToken &cancellation) {
        FixedText<Limits::max_tts_segment_bytes> segment;
        for (std::size_t count = 0; count < Limits::max_tts_segments;
             ++count) {
            bool done = false;
            Error error = source.next(segment, done, cancellation);
            if (error != Error::none || done) {
                return error;
            }
            error = speak(
                segment.data(), segment.size(), sink, cancellation);
            if (error != Error::none) {
                return error;
            }
        }
        bool done = false;
        const Error error = source.next(segment, done, cancellation);
        return error == Error::none && !done ? Error::limit_exceeded : error;
    }
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

class PythonExecutionProvider {
public:
    virtual ~PythonExecutionProvider() = default;
    virtual Error execute(
        const char *source, std::size_t size, PythonExecution &execution,
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
