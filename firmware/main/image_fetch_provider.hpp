#pragma once

#include "chatesp/agent_interfaces.hpp"
#include "http_transport.hpp"

namespace chatesp::image {

class HttpImageFetchProvider final : public agent::ImageFetchProvider {
public:
    explicit HttpImageFetchProvider(transport::HttpTransport &transport)
        : transport_(transport) {}

    agent::Error fetch(
        const agent::ImageFetchRequest &request,
        agent::ByteSink &sink,
        agent::CancellationToken &cancellation) override;

private:
    transport::HttpTransport &transport_;
};

}  // namespace chatesp::image
