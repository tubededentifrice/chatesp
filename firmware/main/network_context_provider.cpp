#include "network_context_provider.hpp"

#include <array>
#include <cstdint>

#include "chatesp/provider_helpers.hpp"
#include "chatesp/transport_policy.hpp"

namespace chatesp {
namespace network {
namespace {

constexpr char kEndpoint[] =
    "https://ipwho.is/?fields=success,city,region,country_code,timezone.offset";
constexpr char kJsonContentType[] = "application/json";
constexpr const char *kJsonTypes[] = {kJsonContentType};
constexpr std::size_t kMaximumResponseBytes = 512;
constexpr agent::RequestPolicy kFastLookupPolicy{
    1'500, 1'500, 1'000, 3'000, 1};

class LocationResponseSink final : public transport::ResponseSink {
public:
    explicit LocationResponseSink(provider::BoundedResponseBuffer &buffer)
        : buffer_(buffer) {}

    agent::Error begin(
        int status, const char *content_type,
        std::int64_t content_length) override {
        buffer_.reset();
        const agent::Error status_error = transport::map_http_status(status);
        if (status_error != agent::Error::none) {
            return status_error;
        }
        if (!transport::content_type_matches(
                content_type, kJsonContentType)) {
            return agent::Error::unsupported_media;
        }
        return content_length <=
                static_cast<std::int64_t>(buffer_.capacity())
            ? agent::Error::none
            : agent::Error::response_too_large;
    }

    agent::Error write(
        const std::uint8_t *data, std::size_t size) override {
        return buffer_.append(data, size) ? agent::Error::none
                                          : agent::Error::response_too_large;
    }

    agent::Error finish() override {
        return buffer_.size() == 0 ? agent::Error::malformed_response
                                   : agent::Error::none;
    }

    void abort() override { buffer_.reset(); }

private:
    provider::BoundedResponseBuffer &buffer_;
};

}  // namespace

agent::Error NetworkContextProvider::lookup(
    agent::IpLocationContext &context,
    transport::HttpResponseDate &response_date,
    agent::CancellationToken &cancellation) {
    context.clear();
    response_date.clear();
    std::array<char, kMaximumResponseBytes + 1> storage{};
    provider::BoundedResponseBuffer response(
        storage.data(), kMaximumResponseBytes);
    LocationResponseSink sink(response);

    transport::HttpRequest request;
    request.method = transport::HttpMethod::get;
    request.url = kEndpoint;
    request.headers[0] = {"Accept", kJsonContentType};
    request.header_count = 1;
    request.max_request_bytes = 1;
    request.response = {kJsonTypes, 1, kMaximumResponseBytes};
    request.timeouts = kFastLookupPolicy;
    request.response_date = &response_date;
    agent::Error error = transport_.execute(request, sink, cancellation);
    if (error == agent::Error::none) {
        error = agent::parse_ip_location_response(
            response.data(), response.size(), context);
    }
    return cancellation.cancelled() ? agent::Error::cancelled : error;
}

}  // namespace network
}  // namespace chatesp
