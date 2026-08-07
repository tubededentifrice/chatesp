#include "http_transport.hpp"

#include <algorithm>
#include <climits>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_timer.h"

namespace chatesp {
namespace transport {
namespace {

constexpr std::size_t kTransferBufferBytes = 2'048;

struct ResponseHeaders {
    agent::FixedText<96> content_type;
    agent::FixedText<max_http_url_bytes> location;
    bool invalid = false;
};

std::uint32_t monotonic_ms() {
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1'000ULL);
}

bool text_equal_fold(const char *left, const char *right) {
    if (left == nullptr || right == nullptr) {
        return false;
    }
    while (*left != '\0' && *right != '\0') {
        const auto l = static_cast<unsigned char>(*left++);
        const auto r = static_cast<unsigned char>(*right++);
        if ((l >= 'A' && l <= 'Z' ? l + ('a' - 'A') : l) !=
            (r >= 'A' && r <= 'Z' ? r + ('a' - 'A') : r)) {
            return false;
        }
    }
    return *left == '\0' && *right == '\0';
}

esp_err_t capture_header(esp_http_client_event_t *event) {
    if (event == nullptr || event->event_id != HTTP_EVENT_ON_HEADER ||
        event->user_data == nullptr) {
        return ESP_OK;
    }
    auto &headers = *static_cast<ResponseHeaders *>(event->user_data);
    if (text_equal_fold(event->header_key, "Content-Type")) {
        if (!headers.content_type.assign(event->header_value)) {
            headers.invalid = true;
        }
    } else if (text_equal_fold(event->header_key, "Location")) {
        if (!headers.location.assign(event->header_value)) {
            headers.invalid = true;
        }
    }
    return ESP_OK;
}

agent::Error map_esp_error(esp_err_t error, bool received_headers) {
    if (error == ESP_OK) {
        return agent::Error::none;
    }
    if (error == ESP_ERR_TIMEOUT || error == ESP_ERR_HTTP_CONNECTING ||
        error == ESP_ERR_HTTP_CONNECT) {
        return agent::Error::connect_timeout;
    }
    if (error == ESP_ERR_HTTP_EAGAIN || error == ESP_ERR_HTTP_READ_TIMEOUT) {
        return received_headers ? agent::Error::idle_timeout
                                : agent::Error::first_byte_timeout;
    }
    return agent::Error::disconnected;
}

agent::Error validate_request(const HttpRequest &request) {
    const std::size_t body_size =
        request.body == nullptr ? 0 : request.body->size();
    if (!valid_https_url(request.url) || request.header_count > max_http_headers ||
        request.timeouts.connect_timeout_ms == 0 ||
        request.timeouts.first_byte_timeout_ms == 0 ||
        request.timeouts.idle_timeout_ms == 0 ||
        request.timeouts.total_timeout_ms == 0) {
        return agent::Error::invalid_argument;
    }
    const agent::Error limit_error = validate_transfer_limits(
        body_size, request.max_request_bytes,
        request.response.max_response_bytes);
    if (limit_error != agent::Error::none) {
        return limit_error;
    }
    if (body_size > INT_MAX) {
        return agent::Error::limit_exceeded;
    }
    if (request.method == HttpMethod::get &&
        request.body != nullptr && request.body->size() != 0) {
        return agent::Error::invalid_argument;
    }
    if (request.allow_image_redirects) {
        if (request.method != HttpMethod::get || request.max_redirects == 0 ||
            request.max_redirects > agent::Limits::max_image_redirects) {
            return agent::Error::invalid_argument;
        }
    } else if (request.max_redirects != 0) {
        return agent::Error::invalid_argument;
    }
    for (std::size_t index = 0; index < request.header_count; ++index) {
        if (!valid_header_name(request.headers[index].name) ||
            !valid_header_value(request.headers[index].value)) {
            return agent::Error::invalid_argument;
        }
        if (request.allow_image_redirects &&
            !header_allowed_on_redirect(request.headers[index].name)) {
            return agent::Error::invalid_argument;
        }
    }
    return agent::Error::none;
}

agent::Error check_cancel_or_total(
    agent::CancellationToken &cancellation, const ElapsedTimer &timer,
    const agent::RequestPolicy &policy) {
    if (cancellation.cancelled()) {
        return agent::Error::cancelled;
    }
    return timer.expired(monotonic_ms(), policy.total_timeout_ms)
               ? agent::Error::total_timeout
               : agent::Error::none;
}

int phase_timeout(
    const ElapsedTimer &timer, std::uint32_t total_limit_ms,
    std::uint32_t phase_limit_ms) {
    const std::uint32_t spent = timer.elapsed_ms(monotonic_ms());
    const std::uint32_t remaining =
        spent >= total_limit_ms ? 1 : total_limit_ms - spent;
    const std::uint32_t selected =
        remaining < phase_limit_ms ? remaining : phase_limit_ms;
    return static_cast<int>(selected > INT_MAX ? INT_MAX : selected);
}

}  // namespace

agent::Error HttpTransport::execute(
    const HttpRequest &request, ResponseSink &sink,
    agent::CancellationToken &cancellation) {
    agent::Error result = validate_request(request);
    if (result != agent::Error::none) {
        return result;
    }
    const ElapsedTimer total_timer{monotonic_ms()};
    const std::size_t body_size =
        request.body == nullptr ? 0 : request.body->size();
    agent::FixedText<max_http_url_bytes> current_url;
    if (!current_url.assign(request.url)) {
        return agent::Error::limit_exceeded;
    }

    for (std::uint8_t redirect_count = 0;; ++redirect_count) {
        result = check_cancel_or_total(cancellation, total_timer, request.timeouts);
        if (result != agent::Error::none) {
            return result;
        }

        ResponseHeaders response_headers;
        esp_http_client_config_t config{};
        config.url = current_url.c_str();
        config.auth_type = HTTP_AUTH_TYPE_NONE;
        config.user_agent = "ChatESP/0.1";
        config.method = request.method == HttpMethod::get ? HTTP_METHOD_GET
                                                          : HTTP_METHOD_POST;
        config.timeout_ms = phase_timeout(
            total_timer, request.timeouts.total_timeout_ms,
            request.timeouts.connect_timeout_ms);
        config.disable_auto_redirect = true;
        config.max_authorization_retries = -1;
        config.event_handler = capture_header;
        config.transport_type = HTTP_TRANSPORT_OVER_SSL;
        config.buffer_size = static_cast<int>(kTransferBufferBytes);
        config.buffer_size_tx = static_cast<int>(kTransferBufferBytes);
        config.user_data = &response_headers;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == nullptr) {
            return agent::Error::disconnected;
        }
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            active_ = client;
        }

        for (std::size_t index = 0; index < request.header_count; ++index) {
            if (esp_http_client_set_header(
                    client, request.headers[index].name,
                    request.headers[index].value) != ESP_OK) {
                result = agent::Error::invalid_argument;
                break;
            }
        }

        if (result == agent::Error::none && request.body != nullptr) {
            result = request.body->reset();
        }
        if (result == agent::Error::none) {
            result = map_esp_error(
                esp_http_client_open(
                    client,
                    request.body == nullptr
                        ? 0
                        : static_cast<int>(body_size)),
                false);
        }

        std::array<std::uint8_t, kTransferBufferBytes> buffer{};
        std::size_t sent = 0;
        while (result == agent::Error::none && request.body != nullptr &&
               sent < body_size) {
            result = check_cancel_or_total(
                cancellation, total_timer, request.timeouts);
            std::size_t read_size = 0;
            if (result == agent::Error::none) {
                result = request.body->read(
                    buffer.data(), buffer.size(), read_size);
            }
            if (result == agent::Error::none &&
                (read_size == 0 || read_size > body_size - sent)) {
                result = agent::Error::invalid_argument;
            }
            if (result == agent::Error::none) {
                std::size_t chunk_sent = 0;
                while (result == agent::Error::none &&
                       chunk_sent < read_size) {
                    result = check_cancel_or_total(
                        cancellation, total_timer, request.timeouts);
                    if (result != agent::Error::none) {
                        break;
                    }
                    const int written = esp_http_client_write(
                        client,
                        reinterpret_cast<const char *>(buffer.data()) +
                            chunk_sent,
                        static_cast<int>(read_size - chunk_sent));
                    if (written <= 0) {
                        result = agent::Error::disconnected;
                    } else {
                        chunk_sent += static_cast<std::size_t>(written);
                    }
                }
                sent += chunk_sent;
            }
        }

        if (result == agent::Error::none) {
            esp_http_client_set_timeout_ms(
                client,
                phase_timeout(
                    total_timer, request.timeouts.total_timeout_ms,
                    request.timeouts.first_byte_timeout_ms));
            const std::int64_t fetched = esp_http_client_fetch_headers(client);
            if (fetched < 0) {
                result = fetched == -ESP_ERR_HTTP_EAGAIN
                             ? agent::Error::first_byte_timeout
                             : agent::Error::disconnected;
            } else if (response_headers.invalid) {
                result = agent::Error::limit_exceeded;
            }
        }

        const int status = esp_http_client_get_status_code(client);
        const std::int64_t content_length =
            esp_http_client_get_content_length(client);
        const bool redirect =
            result == agent::Error::none && is_redirect_status(status);
        if (redirect) {
            if (!request.allow_image_redirects ||
                redirect_count >= request.max_redirects ||
                !valid_https_url(response_headers.location.c_str()) ||
                !current_url.assign(response_headers.location.c_str())) {
                result = agent::Error::malformed_response;
            }
        } else if (result == agent::Error::none) {
            result = validate_response_head(
                status, response_headers.content_type.c_str(), content_length,
                request.response);
        }

        if (!redirect && result == agent::Error::none) {
            bool output_started = false;
            result = sink.begin(
                status, response_headers.content_type.c_str(), content_length);
            output_started = result == agent::Error::none;
            ResponseBudget budget{request.response.max_response_bytes};
            esp_http_client_set_timeout_ms(
                client,
                phase_timeout(
                    total_timer, request.timeouts.total_timeout_ms,
                    request.timeouts.idle_timeout_ms));
            while (result == agent::Error::none) {
                result = check_cancel_or_total(
                    cancellation, total_timer, request.timeouts);
                if (result != agent::Error::none) {
                    break;
                }
                const int read = esp_http_client_read(
                    client, reinterpret_cast<char *>(buffer.data()),
                    static_cast<int>(buffer.size()));
                if (read == 0) {
                    result = validate_response_completion(
                        content_length, budget.used(),
                        esp_http_client_is_complete_data_received(client));
                    break;
                }
                if (read < 0) {
                    result = read == -ESP_ERR_HTTP_EAGAIN
                                 ? agent::Error::idle_timeout
                                 : agent::Error::disconnected;
                    break;
                }
                if (!budget.accept(static_cast<std::size_t>(read))) {
                    result = agent::Error::limit_exceeded;
                    break;
                }
                result = sink.write(
                    buffer.data(), static_cast<std::size_t>(read));
            }
            if (result == agent::Error::none) {
                result = sink.finish();
            }
            if (output_started && result != agent::Error::none) {
                sink.abort();
            }
        }

        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            active_ = nullptr;
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
        }

        if (redirect && result == agent::Error::none) {
            continue;
        }
        return cancellation.cancelled() ? agent::Error::cancelled : result;
    }
}

void HttpTransport::cancel_active() {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (active_ != nullptr) {
        esp_http_client_cancel_request(active_);
    }
}

agent::Error MemoryBodySource::reset() {
    if (size_ != 0 && data_ == nullptr) {
        return agent::Error::invalid_argument;
    }
    offset_ = 0;
    return agent::Error::none;
}

agent::Error MemoryBodySource::read(
    std::uint8_t *buffer, std::size_t capacity, std::size_t &read_size) {
    read_size = 0;
    if (buffer == nullptr || capacity == 0 || offset_ > size_) {
        return agent::Error::invalid_argument;
    }
    read_size = std::min(capacity, size_ - offset_);
    if (read_size != 0) {
        std::memcpy(buffer, data_ + offset_, read_size);
        offset_ += read_size;
    }
    return agent::Error::none;
}

}  // namespace transport
}  // namespace chatesp
