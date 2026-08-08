#include "cloud_providers.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <new>

#include "chatesp/transport_policy.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "streaming_pcm_response_sink.hpp"

namespace chatesp {
namespace cloud {
namespace {

constexpr char kChatPath[] = "/chat/completions";
constexpr char kTag[] = "cloud_provider";
constexpr char kTranscriptionPath[] = "/audio/transcriptions";
constexpr char kSpeechPath[] = "/audio/speech";
constexpr char kBraveEndpoint[] = "https://api.search.brave.com";
constexpr std::size_t kTranscriptionResponseBytes = 8'192;
constexpr std::size_t kAuthorizationBytes = 520;
constexpr std::size_t kBraveKeyBytes = 512;
constexpr char kJsonContentType[] = "application/json";
constexpr char kEventStreamContentType[] = "text/event-stream";
constexpr const char *kJsonTypes[] = {kJsonContentType};
constexpr const char *kEventStreamTypes[] = {kEventStreamContentType};
constexpr const char *kPcmTypes[] = {"audio/pcm"};
constexpr std::uint32_t kRetryDelayMs = 250;

std::uint32_t monotonic_ms() {
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1'000ULL);
}

void secure_wipe(void *data, std::size_t size) {
    auto *bytes = static_cast<volatile std::uint8_t *>(data);
    while (size-- != 0) {
        *bytes++ = 0;
    }
}

class PsramStorage {
public:
    explicit PsramStorage(std::size_t capacity)
        : capacity_(capacity), data_(static_cast<char *>(heap_caps_malloc(
              capacity + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))) {
        if (data_ != nullptr) {
            data_[0] = '\0';
        }
    }

    ~PsramStorage() {
        if (data_ != nullptr) {
            secure_wipe(data_, capacity_ + 1);
            heap_caps_free(data_);
        }
    }

    PsramStorage(const PsramStorage &) = delete;
    PsramStorage &operator=(const PsramStorage &) = delete;

    [[nodiscard]] char *data() const { return data_; }
    [[nodiscard]] std::size_t capacity() const { return capacity_; }
    [[nodiscard]] bool available() const { return data_ != nullptr; }

private:
    std::size_t capacity_ = 0;
    char *data_ = nullptr;
};

template <typename T>
class PsramObject {
public:
    PsramObject() {
        memory_ = heap_caps_malloc(
            sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (memory_ != nullptr) {
            object_ = new (memory_) T();
        }
    }

    ~PsramObject() {
        if (object_ != nullptr) {
            object_->~T();
            secure_wipe(memory_, sizeof(T));
            heap_caps_free(memory_);
        }
    }

    [[nodiscard]] T *get() const { return object_; }

private:
    void *memory_ = nullptr;
    T *object_ = nullptr;
};

class BufferedResponseSink final : public transport::ResponseSink {
public:
    explicit BufferedResponseSink(provider::BoundedResponseBuffer &buffer)
        : buffer_(buffer) {}

    agent::Error begin(
        int status, const char *content_type,
        std::int64_t content_length) override {
        buffer_.reset();
        if (status < 200 || status >= 300 ||
            !transport::content_type_matches(content_type, kJsonContentType)) {
            return agent::Error::unsupported_media;
        }
        if (content_length > static_cast<std::int64_t>(buffer_.capacity())) {
            return agent::Error::limit_exceeded;
        }
        return agent::Error::none;
    }

    agent::Error write(
        const std::uint8_t *data, std::size_t size) override {
        return buffer_.append(data, size) ? agent::Error::none
                                          : agent::Error::limit_exceeded;
    }

    agent::Error finish() override {
        return buffer_.size() == 0 ? agent::Error::malformed_response
                                   : agent::Error::none;
    }

    void abort() override { buffer_.reset(); }

private:
    provider::BoundedResponseBuffer &buffer_;
};

class ChatSseResponseSink final : public transport::ResponseSink {
public:
    ChatSseResponseSink(
        agent::OpenRouterSseParser &parser, agent::ChatTurn &turn,
        agent::CancellationToken &cancellation)
        : parser_(parser), turn_(turn), cancellation_(cancellation) {}

    agent::Error begin(
        int status, const char *content_type,
        std::int64_t content_length) override {
        parser_.reset();
        turn_.clear();
        const agent::Error status_error = transport::map_http_status(status);
        if (status_error != agent::Error::none) {
            return status_error;
        }
        if (!transport::content_type_matches(
                content_type, kEventStreamContentType)) {
            return agent::Error::unsupported_media;
        }
        if (content_length >
            static_cast<std::int64_t>(agent::Limits::max_chat_response_bytes)) {
            return agent::Error::limit_exceeded;
        }
        return cancellation_.cancelled() ? agent::Error::cancelled
                                         : agent::Error::none;
    }

    agent::Error write(
        const std::uint8_t *data, std::size_t size) override {
        if (size != 0 && data == nullptr) {
            return agent::Error::invalid_argument;
        }
        if (cancellation_.cancelled()) {
            return agent::Error::cancelled;
        }
        if (!first_response_byte_ && size != 0) {
            first_response_byte_ = true;
            ESP_LOGI(kTag, "Phase event: first model byte");
        }
        const agent::Error error = parser_.feed(
            reinterpret_cast<const char *>(data), size);
        output_started_ = output_started_ || parser_.output_started();
        if (error != agent::Error::none) {
            return error;
        }
        return cancellation_.cancelled() ? agent::Error::cancelled
                                         : agent::Error::none;
    }

    agent::Error finish() override {
        const agent::Error error = parser_.finish();
        output_started_ = output_started_ || parser_.output_started();
        if (error != agent::Error::none) {
            return error;
        }
        turn_ = parser_.turn();
        return cancellation_.cancelled() ? agent::Error::cancelled
                                         : agent::Error::none;
    }

    void abort() override {
        parser_.reset();
        turn_.clear();
    }

    [[nodiscard]] bool output_started() const { return output_started_; }

private:
    agent::OpenRouterSseParser &parser_;
    agent::ChatTurn &turn_;
    agent::CancellationToken &cancellation_;
    bool output_started_ = false;
    bool first_response_byte_ = false;
};

class WavBodySource final : public transport::BodySource {
public:
    explicit WavBodySource(provider::WavPcmStream &stream) : stream_(stream) {}

    [[nodiscard]] std::size_t size() const override { return stream_.size(); }
    agent::Error reset() override {
        stream_.reset();
        return agent::Error::none;
    }
    agent::Error read(
        std::uint8_t *buffer, std::size_t capacity,
        std::size_t &read_size) override {
        return stream_.read(buffer, capacity, read_size);
    }

private:
    provider::WavPcmStream &stream_;
};

bool retry_delay(
    std::uint32_t duration_ms,
    agent::CancellationToken &cancellation) {
    constexpr std::uint32_t slice_ms = 25;
    std::uint32_t waited_ms = 0;
    while (waited_ms < duration_ms) {
        if (cancellation.cancelled()) {
            return false;
        }
        const std::uint32_t delay_ms =
            std::min(slice_ms, duration_ms - waited_ms);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        waited_ms += delay_ms;
    }
    return !cancellation.cancelled();
}

template <typename OutputStarted>
agent::Error execute_with_retry(
    transport::HttpTransport &transport,
    const transport::HttpRequest &request, transport::ResponseSink &sink,
    agent::CancellationToken &cancellation,
    OutputStarted output_started) {
    const std::uint32_t started_ms = monotonic_ms();
    agent::Error error = agent::Error::none;
    for (std::uint8_t completed = 1;; ++completed) {
        const std::uint32_t elapsed_ms = monotonic_ms() - started_ms;
        if (elapsed_ms >= request.timeouts.total_timeout_ms) {
            return agent::Error::total_timeout;
        }
        transport::HttpRequest attempt = request;
        attempt.timeouts.total_timeout_ms =
            request.timeouts.total_timeout_ms - elapsed_ms;
        error = transport.execute(attempt, sink, cancellation);
        if (!agent::retry_allowed(
                error, completed, output_started(), request.timeouts)) {
            return error;
        }
        ESP_LOGI(
            kTag, "Network retry count: %u",
            static_cast<unsigned>(completed));
        const std::uint32_t after_attempt_ms = monotonic_ms() - started_ms;
        if (after_attempt_ms >= request.timeouts.total_timeout_ms ||
            request.timeouts.total_timeout_ms - after_attempt_ms <=
                kRetryDelayMs) {
            return agent::Error::total_timeout;
        }
        if (!retry_delay(kRetryDelayMs, cancellation)) {
            return agent::Error::cancelled;
        }
    }
}

agent::Error execute_buffered_with_retry(
    transport::HttpTransport &transport,
    const transport::HttpRequest &request, transport::ResponseSink &sink,
    agent::CancellationToken &cancellation) {
    return execute_with_retry(
        transport, request, sink, cancellation, []() { return false; });
}

agent::Error require_network(
    network::NetworkManager &network,
    agent::CancellationToken &cancellation) {
    if (cancellation.cancelled()) {
        return agent::Error::cancelled;
    }
    return network.connected() ? agent::Error::none
                               : agent::Error::disconnected;
}

agent::Error make_openrouter_url(
    const OpenRouterConnectionView &connection, const char *path,
    agent::FixedText<agent::Limits::max_url_bytes> &url) {
    const agent::Error error = provider::build_api_url(
        connection.endpoint, connection.endpoint_size, path, url);
    if (error != agent::Error::none) {
        return error;
    }
    return transport::valid_https_url(url.c_str()) ? agent::Error::none
                                                   : agent::Error::invalid_argument;
}

agent::Error make_brave_url(
    const agent::SearchRequestTarget &target,
    agent::FixedText<agent::Limits::max_url_bytes> &url) {
    const agent::Error error = provider::build_api_url(
        kBraveEndpoint, sizeof(kBraveEndpoint) - 1, target.c_str(), url);
    if (error != agent::Error::none) {
        return error;
    }
    return transport::valid_https_url(url.c_str()) ? agent::Error::none
                                                   : agent::Error::invalid_argument;
}

template <typename Results>
agent::Error execute_brave_search(
    transport::HttpTransport &transport, provider::SecretView api_key,
    const char *url, std::size_t response_limit, Results &results,
    agent::CancellationToken &cancellation,
    agent::Error (*parse)(const char *, std::size_t, Results &)) {
    std::array<char, kBraveKeyBytes + 1> key{};
    if (!provider::valid_secret(api_key, kBraveKeyBytes)) {
        return agent::Error::invalid_argument;
    }
    std::memcpy(key.data(), api_key.data, api_key.size);
    PsramStorage storage(response_limit);
    if (!storage.available()) {
        secure_wipe(key.data(), key.size());
        return agent::Error::model_failed;
    }
    provider::BoundedResponseBuffer response(storage.data(), storage.capacity());
    BufferedResponseSink response_sink(response);
    transport::HttpRequest request;
    request.method = transport::HttpMethod::get;
    request.url = url;
    request.headers[0] = {"X-Subscription-Token", key.data()};
    request.headers[1] = {"Accept", kJsonContentType};
    request.header_count = 2;
    request.max_request_bytes = 1;
    request.response = {kJsonTypes, 1, response_limit};
    request.timeouts = agent::search_policy();
    const agent::Error error = execute_buffered_with_retry(
        transport, request, response_sink, cancellation);
    secure_wipe(key.data(), key.size());
    if (error != agent::Error::none) {
        return error;
    }
    return parse(response.data(), response.size(), results);
}

}  // namespace

agent::Error OpenRouterChatProvider::complete(
    const agent::ConversationHistory &history, agent::ChatTurn &turn,
    agent::CancellationToken &cancellation) {
    agent::NullChatTextSink text_sink;
    return complete_streaming(history, turn, text_sink, cancellation);
}

agent::Error OpenRouterChatProvider::complete_streaming(
    const agent::ConversationHistory &history, agent::ChatTurn &turn,
    agent::ChatTextSink &text_sink,
    agent::CancellationToken &cancellation) {
    return complete_answer_streaming(
        history, turn, text_sink, cancellation);
}

agent::Error OpenRouterChatProvider::route_turn(
    const agent::ConversationHistory &history, agent::TurnRoute &route,
    agent::CancellationToken &cancellation) {
    route.clear();
    agent::ChatTurn turn;
    agent::Error error = require_network(network_, cancellation);
    agent::FixedText<agent::Limits::max_url_bytes> url;
    if (error == agent::Error::none) {
        error = make_openrouter_url(connection_, kChatPath, url);
    }
    PsramObject<agent::ChatRequestBody> body;
    PsramObject<agent::OpenRouterSseParser> parser;
    if (error == agent::Error::none &&
        (body.get() == nullptr || parser.get() == nullptr)) {
        error = agent::Error::model_failed;
    }
    if (error == agent::Error::none) {
        error = agent::build_openrouter_route_request(
            connection_.models, history, tools_, true, *body.get());
    }
    std::array<char, kAuthorizationBytes> authorization{};
    if (error == agent::Error::none) {
        error = provider::build_bearer_header(
            connection_.api_key, authorization.data(), authorization.size());
    }
    if (error == agent::Error::none) {
        transport::MemoryBodySource body_source(
            reinterpret_cast<const std::uint8_t *>(body.get()->data()),
            body.get()->size());
        ChatSseResponseSink response_sink(*parser.get(), turn, cancellation);
        transport::HttpRequest request;
        request.method = transport::HttpMethod::post;
        request.url = url.c_str();
        request.headers[0] = {"Authorization", authorization.data()};
        request.headers[1] = {"Content-Type", kJsonContentType};
        request.headers[2] = {"Accept", kEventStreamContentType};
        request.header_count = 3;
        request.body = &body_source;
        request.max_request_bytes = transport::max_http_request_bytes;
        request.response = {
            kEventStreamTypes, 1, agent::Limits::max_chat_response_bytes};
        request.timeouts = agent::chat_policy();
        error = execute_with_retry(
            transport_, request, response_sink, cancellation,
            [&response_sink]() { return response_sink.output_started(); });
    }
    secure_wipe(authorization.data(), authorization.size());
    if (error == agent::Error::none) {
        if (turn.kind != agent::ChatTurnKind::tool_call) {
            error = agent::Error::malformed_response;
        } else if (turn.tool_call.name.equals("answer_direct")) {
            if (!turn.tool_call.arguments.equals("{}")) {
                error = agent::Error::malformed_response;
            }
        } else if (turn.tool_call.name.equals("search_web")) {
            route.kind = agent::TurnRouteKind::web_search;
            route.tool_call = turn.tool_call;
        } else if (turn.tool_call.name.equals("search_images")) {
            route.kind = agent::TurnRouteKind::image_search;
            route.tool_call = turn.tool_call;
        } else if (tools_.find(turn.tool_call.name.c_str()) == nullptr) {
            error = agent::Error::tool_not_found;
        } else {
            error = agent::Error::malformed_response;
        }
    }
    return cancellation.cancelled() ? agent::Error::cancelled : error;
}

agent::Error OpenRouterChatProvider::complete_answer_streaming(
    const agent::ConversationHistory &history, agent::ChatTurn &turn,
    agent::ChatTextSink &text_sink,
    agent::CancellationToken &cancellation) {
    turn.clear();
    agent::Error error = require_network(network_, cancellation);
    agent::FixedText<agent::Limits::max_url_bytes> url;
    if (error == agent::Error::none) {
        error = make_openrouter_url(connection_, kChatPath, url);
    }
    PsramObject<agent::ChatRequestBody> body;
    PsramObject<agent::OpenRouterSseParser> parser;
    if (error == agent::Error::none &&
        (body.get() == nullptr || parser.get() == nullptr)) {
        error = agent::Error::model_failed;
    }
    if (error == agent::Error::none) {
        error = agent::build_openrouter_answer_request(
            connection_.models, history, true, *body.get());
    }
    std::array<char, kAuthorizationBytes> authorization{};
    if (error == agent::Error::none) {
        error = provider::build_bearer_header(
            connection_.api_key, authorization.data(), authorization.size());
    }
    if (error == agent::Error::none) {
        parser.get()->set_text_sink(&text_sink);
        transport::MemoryBodySource body_source(
            reinterpret_cast<const std::uint8_t *>(body.get()->data()),
            body.get()->size());
        ChatSseResponseSink response_sink(
            *parser.get(), turn, cancellation);
        transport::HttpRequest request;
        request.method = transport::HttpMethod::post;
        request.url = url.c_str();
        request.headers[0] = {"Authorization", authorization.data()};
        request.headers[1] = {"Content-Type", kJsonContentType};
        request.headers[2] = {"Accept", kEventStreamContentType};
        request.header_count = 3;
        request.body = &body_source;
        request.max_request_bytes = transport::max_http_request_bytes;
        request.response = {
            kEventStreamTypes, 1,
            agent::Limits::max_chat_response_bytes};
        request.timeouts = agent::chat_policy();
        error = execute_with_retry(
            transport_, request, response_sink, cancellation,
            [&response_sink]() { return response_sink.output_started(); });
    }
    secure_wipe(authorization.data(), authorization.size());
    return cancellation.cancelled() ? agent::Error::cancelled : error;
}

void OpenRouterChatProvider::cancel_active() { transport_.cancel_active(); }

agent::Error OpenRouterTranscriptionProvider::transcribe(
    const agent::AudioView &audio,
    agent::FixedText<agent::Limits::max_transcript_bytes> &transcript,
    agent::CancellationToken &cancellation) {
    transcript.clear();
    agent::Error error = require_network(network_, cancellation);
    agent::FixedText<agent::Limits::max_url_bytes> url;
    if (error == agent::Error::none) {
        error = make_openrouter_url(connection_, kTranscriptionPath, url);
    }
    agent::MultipartTranscriptionPlan plan;
    if (error == agent::Error::none &&
        audio.size > agent::Limits::max_recording_pcm_bytes) {
        error = agent::Error::limit_exceeded;
    }
    if (error == agent::Error::none) {
        error = agent::build_openrouter_transcription_plan(
            connection_.models, audio.size + 44, plan);
    }
    provider::WavPcmStream stream;
    if (error == agent::Error::none) {
        error = stream.configure(
            reinterpret_cast<const std::uint8_t *>(plan.preamble.data()),
            plan.preamble.size(), audio,
            reinterpret_cast<const std::uint8_t *>(plan.epilogue.data()),
            plan.epilogue.size());
    }
    if (error == agent::Error::none && stream.size() != plan.content_length) {
        error = agent::Error::malformed_response;
    }
    PsramStorage storage(kTranscriptionResponseBytes);
    if (error == agent::Error::none && !storage.available()) {
        error = agent::Error::model_failed;
    }
    provider::BoundedResponseBuffer response(storage.data(), storage.capacity());
    std::array<char, kAuthorizationBytes> authorization{};
    if (error == agent::Error::none) {
        error = provider::build_bearer_header(
            connection_.api_key, authorization.data(), authorization.size());
    }
    if (error == agent::Error::none) {
        WavBodySource body_source(stream);
        BufferedResponseSink response_sink(response);
        transport::HttpRequest request;
        request.method = transport::HttpMethod::post;
        request.url = url.c_str();
        request.headers[0] = {"Authorization", authorization.data()};
        request.headers[1] = {"Content-Type", plan.content_type.c_str()};
        request.headers[2] = {"Accept", kJsonContentType};
        request.header_count = 3;
        request.body = &body_source;
        request.max_request_bytes = transport::max_http_request_bytes;
        request.response = {kJsonTypes, 1, kTranscriptionResponseBytes};
        request.timeouts = agent::transcription_policy();
        error = execute_buffered_with_retry(
            transport_, request, response_sink, cancellation);
    }
    secure_wipe(authorization.data(), authorization.size());
    if (error == agent::Error::none) {
        error = agent::parse_openrouter_transcription_response(
            response.data(), response.size(), transcript);
    }
    return cancellation.cancelled() ? agent::Error::cancelled : error;
}

void OpenRouterTranscriptionProvider::cancel_active() {
    transport_.cancel_active();
}

agent::Error OpenRouterSpeechProvider::speak(
    const char *text, std::size_t size, agent::PcmSink &sink,
    agent::CancellationToken &cancellation) {
    agent::Error error = require_network(network_, cancellation);
    agent::FixedText<agent::Limits::max_url_bytes> url;
    if (error == agent::Error::none) {
        error = make_openrouter_url(connection_, kSpeechPath, url);
    }
    agent::SpeechRequestBody body;
    if (error == agent::Error::none) {
        error = agent::build_openrouter_speech_request(
            connection_.models, text, size, body);
    }
    std::array<char, kAuthorizationBytes> authorization{};
    if (error == agent::Error::none) {
        error = provider::build_bearer_header(
            connection_.api_key, authorization.data(), authorization.size());
    }
    if (error == agent::Error::none) {
        transport::MemoryBodySource body_source(
            reinterpret_cast<const std::uint8_t *>(body.data()), body.size());
        StreamingPcmResponseSink response_sink(sink, cancellation);
        transport::HttpRequest request;
        request.method = transport::HttpMethod::post;
        request.url = url.c_str();
        request.headers[0] = {"Authorization", authorization.data()};
        request.headers[1] = {"Content-Type", kJsonContentType};
        request.headers[2] = {"Accept", "audio/pcm"};
        request.header_count = 3;
        request.body = &body_source;
        request.max_request_bytes = transport::max_http_request_bytes;
        request.response = {
            kPcmTypes, 1, agent::Limits::max_tts_pcm_bytes};
        request.timeouts = agent::speech_policy();
        error = execute_with_retry(
            transport_, request, response_sink, cancellation,
            [&response_sink]() { return response_sink.output_started(); });
        if (error == agent::Error::none) {
            error = response_sink.finish_sequence();
        }
    }
    secure_wipe(authorization.data(), authorization.size());
    secure_wipe(body.data(), body.capacity());
    return cancellation.cancelled() ? agent::Error::cancelled : error;
}

agent::Error OpenRouterSpeechProvider::speak_segments(
    SegmentSource &source, agent::PcmSink &sink,
    agent::CancellationToken &cancellation) {
    agent::Error error = require_network(network_, cancellation);
    agent::FixedText<agent::Limits::max_url_bytes> url;
    if (error == agent::Error::none) {
        error = make_openrouter_url(connection_, kSpeechPath, url);
    }
    std::array<char, kAuthorizationBytes> authorization{};
    if (error == agent::Error::none) {
        error = provider::build_bearer_header(
            connection_.api_key, authorization.data(), authorization.size());
    }

    StreamingPcmResponseSink response_sink(sink, cancellation);
    std::size_t segment_count = 0;
    while (error == agent::Error::none &&
           segment_count < agent::Limits::max_tts_segments) {
        agent::FixedText<agent::Limits::max_tts_segment_bytes> segment;
        bool done = false;
        error = source.next(segment, done, cancellation);
        if (error != agent::Error::none || done) {
            break;
        }

        agent::SpeechRequestBody body;
        error = agent::build_openrouter_speech_request(
            connection_.models, segment.data(), segment.size(), body);
        ESP_LOGI(
            kTag, "Phase event: TTS request %u",
            static_cast<unsigned>(segment_count + 1));
        if (error == agent::Error::none) {
            transport::MemoryBodySource body_source(
                reinterpret_cast<const std::uint8_t *>(body.data()),
                body.size());
            transport::HttpRequest request;
            request.method = transport::HttpMethod::post;
            request.url = url.c_str();
            request.headers[0] = {"Authorization", authorization.data()};
            request.headers[1] = {"Content-Type", kJsonContentType};
            request.headers[2] = {"Accept", "audio/pcm"};
            request.header_count = 3;
            request.body = &body_source;
            request.max_request_bytes = transport::max_http_request_bytes;
            request.response = {
                kPcmTypes, 1, agent::Limits::max_tts_pcm_bytes};
            request.timeouts = agent::speech_policy();
            error = execute_with_retry(
                transport_, request, response_sink, cancellation,
                [&response_sink]() {
                    return response_sink.current_segment_started();
                });
        }
        secure_wipe(body.data(), body.capacity());
        ++segment_count;
    }
    if (error == agent::Error::none) {
        error = segment_count == 0 ? agent::Error::malformed_response
                                   : response_sink.finish_sequence();
    }
    secure_wipe(authorization.data(), authorization.size());
    return cancellation.cancelled() ? agent::Error::cancelled : error;
}

void OpenRouterSpeechProvider::cancel_active() { transport_.cancel_active(); }

agent::Error BraveWebSearchProvider::search(
    const char *query, std::size_t size, agent::WebResults &results,
    agent::CancellationToken &cancellation) {
    results.clear();
    agent::Error error = require_network(network_, cancellation);
    agent::SearchRequestTarget target;
    if (error == agent::Error::none) {
        error = agent::build_brave_web_search_target(
            query, size, options_, target);
    }
    agent::FixedText<agent::Limits::max_url_bytes> url;
    if (error == agent::Error::none) {
        error = make_brave_url(target, url);
    }
    if (error == agent::Error::none) {
        error = execute_brave_search(
            transport_, api_key_, url.c_str(),
            agent::Limits::max_web_response_bytes, results, cancellation,
            agent::parse_brave_web_response);
    }
    return cancellation.cancelled() ? agent::Error::cancelled : error;
}

void BraveWebSearchProvider::cancel_active() { transport_.cancel_active(); }

agent::Error BraveImageSearchProvider::search(
    const char *query, std::size_t size, agent::ImageResults &results,
    agent::CancellationToken &cancellation) {
    results.clear();
    agent::Error error = require_network(network_, cancellation);
    agent::SearchRequestTarget target;
    if (error == agent::Error::none) {
        error = agent::build_brave_image_search_target(
            query, size, options_, target);
    }
    agent::FixedText<agent::Limits::max_url_bytes> url;
    if (error == agent::Error::none) {
        error = make_brave_url(target, url);
    }
    if (error == agent::Error::none) {
        error = execute_brave_search(
            transport_, api_key_, url.c_str(),
            agent::Limits::max_image_search_response_bytes, results,
            cancellation, agent::parse_brave_image_response);
    }
    return cancellation.cancelled() ? agent::Error::cancelled : error;
}

void BraveImageSearchProvider::cancel_active() { transport_.cancel_active(); }

}  // namespace cloud
}  // namespace chatesp
