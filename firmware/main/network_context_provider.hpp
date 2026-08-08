#pragma once

#include "chatesp/agent_interfaces.hpp"
#include "chatesp/ip_location.hpp"
#include "http_transport.hpp"

namespace chatesp {
namespace network {

class NetworkContextProvider {
public:
    explicit NetworkContextProvider(transport::HttpTransport &transport)
        : transport_(transport) {}

    agent::Error lookup(
        agent::IpLocationContext &context,
        transport::HttpResponseDate &response_date,
        agent::CancellationToken &cancellation);

private:
    transport::HttpTransport &transport_;
};

}  // namespace network
}  // namespace chatesp
