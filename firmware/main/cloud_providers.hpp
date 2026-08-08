#pragma once

#include <atomic>
#include <cstddef>
#include <string_view>

#include "chatesp/agent_interfaces.hpp"
#include "chatesp/brave_protocol.hpp"
#include "chatesp/openrouter_protocol.hpp"
#include "chatesp/provider_helpers.hpp"
#include "chatesp/tool_registry.hpp"
#include "chatesp/utc_clock.hpp"
#include "http_transport.hpp"
#include "network_manager.hpp"

namespace chatesp {
namespace cloud {

struct OpenRouterConnectionView {
    const char *endpoint = nullptr;
    std::size_t endpoint_size = 0;
    provider::SecretView api_key{};
    agent::OpenRouterConfig models{};
};

// Connection and key views do not own their bytes. The caller must keep the
// bytes valid and must update them only while no provider request is active.

class OpenRouterChatProvider final : public agent::ChatProvider {
public:
    OpenRouterChatProvider(
        transport::HttpTransport &transport,
        network::NetworkManager &network,
        const OpenRouterConnectionView &connection,
        const agent::ToolRegistry &tools,
        agent::MemoryControlProvider &memory_provider,
        agent::UtcClock &utc_clock,
        std::string_view approximate_location)
        : transport_(transport), network_(network), connection_(connection),
          tools_(tools), memory_provider_(memory_provider), utc_clock_(utc_clock),
          approximate_location_(approximate_location) {}

    agent::Error complete(
        const agent::ConversationHistory &history, agent::ChatTurn &turn,
        agent::CancellationToken &cancellation) override;
    agent::Error complete_streaming(
        const agent::ConversationHistory &history, agent::ChatTurn &turn,
        agent::ChatTextSink &text_sink,
        agent::CancellationToken &cancellation) override;
    agent::Error route_turn(
        const agent::ConversationHistory &history, agent::TurnRoute &route,
        agent::CancellationToken &cancellation) override;
    agent::Error complete_answer_streaming(
        const agent::ConversationHistory &history, agent::ChatTurn &turn,
        agent::ChatTextSink &text_sink,
        agent::CancellationToken &cancellation) override;
    void set_connection(const OpenRouterConnectionView &connection) {
        connection_ = connection;
    }
    void set_approximate_location(std::string_view value) {
        approximate_location_ = value;
    }
    void cancel_active();

private:
    transport::HttpTransport &transport_;
    network::NetworkManager &network_;
    OpenRouterConnectionView connection_;
    const agent::ToolRegistry &tools_;
    agent::MemoryControlProvider &memory_provider_;
    agent::UtcClock &utc_clock_;
    std::string_view approximate_location_;
};

class OpenRouterTranscriptionProvider final
    : public agent::TranscriptionProvider {
public:
    OpenRouterTranscriptionProvider(
        transport::HttpTransport &transport,
        network::NetworkManager &network,
        const OpenRouterConnectionView &connection, agent::UtcClock &utc_clock)
        : transport_(transport), network_(network), connection_(connection),
          utc_clock_(utc_clock) {}

    agent::Error transcribe(
        const agent::AudioView &audio,
        agent::FixedText<agent::Limits::max_transcript_bytes> &transcript,
        agent::CancellationToken &cancellation) override;
    void set_connection(const OpenRouterConnectionView &connection) {
        connection_ = connection;
    }
    void cancel_active();

private:
    transport::HttpTransport &transport_;
    network::NetworkManager &network_;
    OpenRouterConnectionView connection_;
    agent::UtcClock &utc_clock_;
};

class OpenRouterSpeechProvider final : public agent::SpeechProvider {
public:
    OpenRouterSpeechProvider(
        transport::HttpTransport &transport,
        network::NetworkManager &network,
        const OpenRouterConnectionView &connection)
        : transport_(transport), network_(network), connection_(connection) {}

    agent::Error speak(
        const char *text, std::size_t size, agent::PcmSink &sink,
        agent::CancellationToken &cancellation) override;
    agent::Error speak_segments(
        SegmentSource &source, agent::PcmSink &sink,
        agent::CancellationToken &cancellation) override;
    void set_language(agent::SpeechLanguage language) override {
        language_.store(language, std::memory_order_release);
    }
    void set_connection(const OpenRouterConnectionView &connection) {
        connection_ = connection;
    }
    void cancel_active();

private:
    transport::HttpTransport &transport_;
    network::NetworkManager &network_;
    OpenRouterConnectionView connection_;
    std::atomic<agent::SpeechLanguage> language_{
        agent::SpeechLanguage::english};
};

class BraveWebSearchProvider final : public agent::WebSearchProvider {
public:
    BraveWebSearchProvider(
        transport::HttpTransport &transport,
        network::NetworkManager &network, provider::SecretView api_key,
        const agent::BraveSearchOptions &options = {})
        : transport_(transport), network_(network), api_key_(api_key),
          options_(options) {}

    agent::Error search(
        const char *query, std::size_t size, agent::WebResults &results,
        agent::CancellationToken &cancellation) override;
    void set_api_key(provider::SecretView api_key) { api_key_ = api_key; }
    void cancel_active();

private:
    transport::HttpTransport &transport_;
    network::NetworkManager &network_;
    provider::SecretView api_key_;
    agent::BraveSearchOptions options_;
};

class BraveImageSearchProvider final : public agent::ImageSearchProvider {
public:
    BraveImageSearchProvider(
        transport::HttpTransport &transport,
        network::NetworkManager &network, provider::SecretView api_key,
        const agent::BraveSearchOptions &options = {})
        : transport_(transport), network_(network), api_key_(api_key),
          options_(options) {}

    agent::Error search(
        const char *query, std::size_t size, agent::ImageResults &results,
        agent::CancellationToken &cancellation) override;
    void set_api_key(provider::SecretView api_key) { api_key_ = api_key; }
    void cancel_active();

private:
    transport::HttpTransport &transport_;
    network::NetworkManager &network_;
    provider::SecretView api_key_;
    agent::BraveSearchOptions options_;
};

}  // namespace cloud
}  // namespace chatesp
