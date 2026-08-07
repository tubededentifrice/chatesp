#pragma once

#include <cstddef>

#include "chatesp/agent_interfaces.hpp"
#include "chatesp/brave_protocol.hpp"
#include "chatesp/openrouter_protocol.hpp"
#include "chatesp/provider_helpers.hpp"
#include "chatesp/tool_registry.hpp"
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

class OpenRouterChatProvider final : public agent::ChatProvider {
public:
    OpenRouterChatProvider(
        transport::HttpTransport &transport,
        network::NetworkManager &network,
        const OpenRouterConnectionView &connection,
        const agent::ToolRegistry &tools)
        : transport_(transport), network_(network), connection_(connection),
          tools_(tools) {}

    agent::Error complete(
        const agent::ConversationHistory &history, agent::ChatTurn &turn,
        agent::CancellationToken &cancellation) override;
    void cancel_active();

private:
    transport::HttpTransport &transport_;
    network::NetworkManager &network_;
    OpenRouterConnectionView connection_;
    const agent::ToolRegistry &tools_;
};

class OpenRouterTranscriptionProvider final
    : public agent::TranscriptionProvider {
public:
    OpenRouterTranscriptionProvider(
        transport::HttpTransport &transport,
        network::NetworkManager &network,
        const OpenRouterConnectionView &connection)
        : transport_(transport), network_(network), connection_(connection) {}

    agent::Error transcribe(
        const agent::AudioView &audio,
        agent::FixedText<agent::Limits::max_transcript_bytes> &transcript,
        agent::CancellationToken &cancellation) override;
    void cancel_active();

private:
    transport::HttpTransport &transport_;
    network::NetworkManager &network_;
    OpenRouterConnectionView connection_;
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
    void cancel_active();

private:
    transport::HttpTransport &transport_;
    network::NetworkManager &network_;
    OpenRouterConnectionView connection_;
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
    void cancel_active();

private:
    transport::HttpTransport &transport_;
    network::NetworkManager &network_;
    provider::SecretView api_key_;
    agent::BraveSearchOptions options_;
};

}  // namespace cloud
}  // namespace chatesp
