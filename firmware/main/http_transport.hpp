#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "chatesp/agent_interfaces.hpp"
#include "chatesp/fixed_text.hpp"
#include "chatesp/transport_policy.hpp"
#include "esp_http_client.h"

namespace chatesp {
namespace transport {

struct HttpHeader {
    const char *name = nullptr;
    const char *value = nullptr;
};

class BodySource {
public:
    virtual ~BodySource() = default;
    [[nodiscard]] virtual std::size_t size() const = 0;
    virtual agent::Error reset() = 0;
    virtual agent::Error read(
        std::uint8_t *buffer, std::size_t capacity, std::size_t &read_size) = 0;
};

class ResponseSink {
public:
    virtual ~ResponseSink() = default;
    virtual agent::Error begin(
        int status, const char *content_type, std::int64_t content_length) = 0;
    virtual agent::Error write(const std::uint8_t *data, std::size_t size) = 0;
    virtual agent::Error finish() = 0;
    virtual void abort() = 0;
};

struct HttpRequest {
    HttpMethod method = HttpMethod::get;
    const char *url = nullptr;
    std::array<HttpHeader, max_http_headers> headers{};
    std::size_t header_count = 0;
    BodySource *body = nullptr;
    std::size_t max_request_bytes = 0;
    ResponsePolicy response{};
    agent::RequestPolicy timeouts{};
    bool allow_image_redirects = false;
    std::uint8_t max_redirects = 0;
};

class HttpTransport {
public:
    agent::Error execute(
        const HttpRequest &request, ResponseSink &sink,
        agent::CancellationToken &cancellation);

    // This is safe to call from the action-button task.
    void cancel_active();

private:
    std::mutex active_mutex_;
    esp_http_client_handle_t active_ = nullptr;
};

class MemoryBodySource final : public BodySource {
public:
    MemoryBodySource(const std::uint8_t *data, std::size_t size)
        : data_(data), size_(size) {}

    [[nodiscard]] std::size_t size() const override { return size_; }
    agent::Error reset() override;
    agent::Error read(
        std::uint8_t *buffer, std::size_t capacity,
        std::size_t &read_size) override;

private:
    const std::uint8_t *data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t offset_ = 0;
};

}  // namespace transport
}  // namespace chatesp
