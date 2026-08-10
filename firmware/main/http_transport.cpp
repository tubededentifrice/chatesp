#include "http_transport.hpp"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstring>

#include "ble_provisioning.hpp"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace chatesp {
namespace transport {
namespace {

constexpr std::size_t kTransferBufferBytes = 2'048;
constexpr std::size_t kProxyCommonHeaderBytes = 10;
constexpr std::size_t kProxyDataHeaderBytes = 14;
constexpr std::uint8_t kProxyVersion = 1;
constexpr std::uint8_t kProxyRequestStart = 1;
constexpr std::uint8_t kProxyRequestData = 2;
constexpr std::uint8_t kProxyRequestEnd = 3;
constexpr std::uint8_t kProxyResponseHead = 0x11;
constexpr std::uint8_t kProxyResponseData = 0x12;
constexpr std::uint8_t kProxyResponseEnd = 0x13;
constexpr std::uint8_t kProxyResponseError = 0x14;
constexpr std::uint32_t kProxyFrameTimeoutMs = 10'000;
std::mutex s_ble_proxy_request_mutex;
std::atomic<std::uint32_t> s_next_proxy_request_id{1};

std::uint32_t monotonic_ms();

void write_u16(std::uint8_t *output, std::uint16_t value) {
    output[0] = static_cast<std::uint8_t>(value >> 8U);
    output[1] = static_cast<std::uint8_t>(value);
}

void write_u32(std::uint8_t *output, std::uint32_t value) {
    output[0] = static_cast<std::uint8_t>(value >> 24U);
    output[1] = static_cast<std::uint8_t>(value >> 16U);
    output[2] = static_cast<std::uint8_t>(value >> 8U);
    output[3] = static_cast<std::uint8_t>(value);
}

std::uint16_t read_u16(const std::uint8_t *input) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(input[0]) << 8U) | input[1]);
}

std::uint32_t read_u32(const std::uint8_t *input) {
    return (static_cast<std::uint32_t>(input[0]) << 24U) |
        (static_cast<std::uint32_t>(input[1]) << 16U) |
        (static_cast<std::uint32_t>(input[2]) << 8U) |
        static_cast<std::uint32_t>(input[3]);
}

std::uint64_t read_u64(const std::uint8_t *input) {
    return (static_cast<std::uint64_t>(read_u32(input)) << 32U) |
        read_u32(input + 4);
}

void write_proxy_common(
    std::uint8_t *output, std::uint8_t type, std::uint32_t request_id) {
    std::memcpy(output, "CEPX", 4);
    output[4] = kProxyVersion;
    output[5] = type;
    write_u32(output + 6, request_id);
}

bool valid_proxy_common(
    const std::uint8_t *frame, std::size_t size, std::uint32_t request_id) {
    return size >= kProxyCommonHeaderBytes &&
        std::memcmp(frame, "CEPX", 4) == 0 &&
        frame[4] == kProxyVersion && read_u32(frame + 6) == request_id;
}

std::uint32_t bounded_proxy_timeout(
    const ElapsedTimer &timer, const agent::RequestPolicy &policy) {
    const std::uint32_t elapsed = timer.elapsed_ms(monotonic_ms());
    if (elapsed >= policy.total_timeout_ms) {
        return 1;
    }
    const std::uint32_t remaining = policy.total_timeout_ms - elapsed;
    return remaining < kProxyFrameTimeoutMs ? remaining : kProxyFrameTimeoutMs;
}

agent::Error proxy_link_error(esp_err_t error) {
    return error == ESP_ERR_TIMEOUT ? agent::Error::idle_timeout
                                    : agent::Error::disconnected;
}

class BleProxyRequestSender {
public:
    BleProxyRequestSender(
        std::uint32_t request_id, const ElapsedTimer &timer,
        const agent::RequestPolicy &policy)
        : request_id_(request_id), timer_(timer), policy_(policy) {}

    agent::Error send(const std::uint8_t *data, std::size_t size) {
        if (size != 0 && data == nullptr) {
            return agent::Error::invalid_argument;
        }
        const agent::Error timer_error = cancellation_error();
        if (timer_error != agent::Error::none) {
            return timer_error;
        }
        const std::size_t capacity =
            ble_provisioning::http_proxy_frame_capacity();
        if (capacity <= kProxyDataHeaderBytes) {
            return agent::Error::disconnected;
        }
        const std::size_t chunk_capacity = capacity - kProxyDataHeaderBytes;
        while (size != 0) {
            const std::size_t chunk = size < chunk_capacity
                ? size
                : chunk_capacity;
            std::array<std::uint8_t,
                ble_provisioning::kMaximumHttpProxyFrameSize> frame{};
            write_proxy_common(
                frame.data(), kProxyRequestData, request_id_);
            write_u32(frame.data() + 10, offset_);
            std::memcpy(frame.data() + kProxyDataHeaderBytes, data, chunk);
            const esp_err_t result = ble_provisioning::send_http_proxy_frame(
                frame.data(), kProxyDataHeaderBytes + chunk,
                bounded_proxy_timeout(timer_, policy_));
            std::fill(frame.begin(), frame.end(), 0);
            if (result != ESP_OK) {
                return proxy_link_error(result);
            }
            offset_ += static_cast<std::uint32_t>(chunk);
            data += chunk;
            size -= chunk;
        }
        return agent::Error::none;
    }

    [[nodiscard]] std::uint32_t offset() const { return offset_; }

private:
    agent::Error cancellation_error() const {
        return timer_.expired(monotonic_ms(), policy_.total_timeout_ms)
            ? agent::Error::total_timeout
            : agent::Error::none;
    }

    std::uint32_t request_id_ = 0;
    const ElapsedTimer &timer_;
    const agent::RequestPolicy &policy_;
    std::uint32_t offset_ = 0;
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
        return agent::Error::request_too_large;
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

bool extract_origin(
    const char *url, agent::FixedText<max_http_url_bytes> &origin) {
    origin.clear();
    if (url == nullptr || std::strncmp(url, "https://", 8) != 0) {
        return false;
    }
    std::size_t size = 8;
    while (url[size] != '\0' && url[size] != '/' &&
           size <= max_http_url_bytes) {
        ++size;
    }
    return size > 8 && size <= max_http_url_bytes && origin.assign(url, size);
}

agent::Error execute_ble_proxy(
    const HttpRequest &request, ResponseSink &sink,
    agent::CancellationToken &cancellation) {
    std::lock_guard<std::mutex> proxy_lock(s_ble_proxy_request_mutex);
    if (!ble_provisioning::http_proxy_available()) {
        return agent::Error::disconnected;
    }
    const ElapsedTimer timer{monotonic_ms()};
    const std::size_t body_size = request.body == nullptr
        ? 0
        : request.body->size();
    std::size_t metadata_size = std::strlen(request.url);
    for (std::size_t index = 0; index < request.header_count; ++index) {
        metadata_size += 3 + std::strlen(request.headers[index].name) +
            std::strlen(request.headers[index].value);
    }
    if (body_size > UINT32_MAX || metadata_size > UINT32_MAX ||
        metadata_size + body_size > UINT32_MAX ||
        request.response.max_response_bytes > UINT32_MAX ||
        std::strlen(request.url) > UINT16_MAX) {
        return agent::Error::request_too_large;
    }

    std::uint32_t request_id = s_next_proxy_request_id.fetch_add(
        1, std::memory_order_relaxed);
    if (request_id == 0) {
        request_id = s_next_proxy_request_id.fetch_add(
            1, std::memory_order_relaxed);
    }
    ESP_LOGI(
        "http_transport",
        "BLE proxy request started: request=%u envelope=%u body=%u",
        static_cast<unsigned>(request_id),
        static_cast<unsigned>(metadata_size + body_size),
        static_cast<unsigned>(body_size));
    ble_provisioning::begin_http_proxy_exchange();
    std::array<std::uint8_t, 36> start{};
    write_proxy_common(start.data(), kProxyRequestStart, request_id);
    write_u32(
        start.data() + 10,
        static_cast<std::uint32_t>(metadata_size + body_size));
    write_u32(start.data() + 14, static_cast<std::uint32_t>(body_size));
    write_u32(
        start.data() + 18,
        static_cast<std::uint32_t>(request.response.max_response_bytes));
    write_u32(start.data() + 22, request.timeouts.total_timeout_ms);
    start[26] = request.method == HttpMethod::post ? 1 : 0;
    start[27] = request.allow_image_redirects ? 1 : 0;
    start[28] = request.max_redirects;
    start[29] = static_cast<std::uint8_t>(request.header_count);
    write_u16(
        start.data() + 30,
        static_cast<std::uint16_t>(std::strlen(request.url)));
    write_u32(start.data() + 32, static_cast<std::uint32_t>(metadata_size));
    esp_err_t link_result = ble_provisioning::send_http_proxy_frame(
        start.data(), start.size(), bounded_proxy_timeout(timer, request.timeouts));
    std::fill(start.begin(), start.end(), 0);
    if (link_result != ESP_OK) {
        ESP_LOGW(
            "http_transport",
            "BLE proxy request start failed: request=%u category=%s",
            static_cast<unsigned>(request_id), esp_err_to_name(link_result));
        return proxy_link_error(link_result);
    }

    BleProxyRequestSender sender(request_id, timer, request.timeouts);
    agent::Error result = sender.send(
        reinterpret_cast<const std::uint8_t *>(request.url),
        std::strlen(request.url));
    for (std::size_t index = 0;
         result == agent::Error::none && index < request.header_count;
         ++index) {
        const std::size_t name_size =
            std::strlen(request.headers[index].name);
        const std::size_t value_size =
            std::strlen(request.headers[index].value);
        std::array<std::uint8_t, 3> prefix{};
        prefix[0] = static_cast<std::uint8_t>(name_size);
        write_u16(prefix.data() + 1, static_cast<std::uint16_t>(value_size));
        result = sender.send(prefix.data(), prefix.size());
        if (result == agent::Error::none) {
            result = sender.send(
                reinterpret_cast<const std::uint8_t *>(
                    request.headers[index].name),
                name_size);
        }
        if (result == agent::Error::none) {
            result = sender.send(
                reinterpret_cast<const std::uint8_t *>(
                    request.headers[index].value),
                value_size);
        }
    }
    if (result == agent::Error::none && request.body != nullptr) {
        result = request.body->reset();
    }
    std::array<std::uint8_t, kTransferBufferBytes> body_buffer{};
    std::size_t sent_body = 0;
    while (result == agent::Error::none && sent_body < body_size) {
        if (cancellation.cancelled()) {
            result = agent::Error::cancelled;
            break;
        }
        std::size_t read_size = 0;
        result = request.body->read(
            body_buffer.data(), body_buffer.size(), read_size);
        if (result == agent::Error::none &&
            (read_size == 0 || read_size > body_size - sent_body)) {
            result = agent::Error::invalid_argument;
        }
        if (result == agent::Error::none) {
            result = sender.send(body_buffer.data(), read_size);
            sent_body += read_size;
        }
    }
    std::fill(body_buffer.begin(), body_buffer.end(), 0);
    if (result != agent::Error::none) {
        return result;
    }
    std::array<std::uint8_t, kProxyCommonHeaderBytes> end{};
    write_proxy_common(end.data(), kProxyRequestEnd, request_id);
    link_result = ble_provisioning::send_http_proxy_frame(
        end.data(), end.size(), bounded_proxy_timeout(timer, request.timeouts));
    if (link_result != ESP_OK) {
        return proxy_link_error(link_result);
    }

    bool head_received = false;
    bool output_started = false;
    std::size_t response_bytes = 0;
    std::int64_t content_length = -1;
    ResponseBudget budget{request.response.max_response_bytes};
    ResponseBodyProgress body_progress{monotonic_ms()};
    std::array<std::uint8_t,
        ble_provisioning::kMaximumHttpProxyFrameSize> frame{};
    while (result == agent::Error::none) {
        if (cancellation.cancelled()) {
            result = agent::Error::cancelled;
            break;
        }
        if (timer.expired(monotonic_ms(), request.timeouts.total_timeout_ms)) {
            result = agent::Error::total_timeout;
            break;
        }
        std::size_t frame_size = 0;
        link_result = ble_provisioning::receive_http_proxy_frame(
            frame.data(), frame.size(), &frame_size, 250);
        if (link_result == ESP_ERR_TIMEOUT) {
            if (body_progress.expired(
                    monotonic_ms(), request.timeouts)) {
                result = body_progress.timeout_error();
            }
            continue;
        }
        if (link_result != ESP_OK ||
            !valid_proxy_common(frame.data(), frame_size, request_id)) {
            result = agent::Error::disconnected;
            break;
        }
        const std::uint8_t type = frame[5];
        if (type == kProxyResponseHead) {
            if (head_received || frame_size < 22) {
                result = agent::Error::malformed_response;
                break;
            }
            const std::size_t content_type_size = frame[20];
            const std::size_t date_size = frame[21];
            if (content_type_size > 95 || date_size > 63 ||
                frame_size != 22 + content_type_size + date_size) {
                result = agent::Error::malformed_response;
                break;
            }
            std::array<char, 96> content_type{};
            std::array<char, 64> date{};
            std::memcpy(
                content_type.data(), frame.data() + 22,
                content_type_size);
            std::memcpy(
                date.data(), frame.data() + 22 + content_type_size,
                date_size);
            const int status = read_u16(frame.data() + 10);
            const std::uint64_t encoded_length = read_u64(frame.data() + 12);
            if (encoded_length > static_cast<std::uint64_t>(INT64_MAX)) {
                result = agent::Error::malformed_response;
                break;
            }
            content_length = static_cast<std::int64_t>(encoded_length);
            result = validate_response_head(
                status, content_type.data(), content_length,
                request.response);
            if (result == agent::Error::none) {
                result = sink.begin(
                    status, content_type.data(), content_length);
                output_started = result == agent::Error::none;
            }
            if (result == agent::Error::none &&
                request.response_date != nullptr && date_size != 0 &&
                request.response_date->value.assign(date.data(), date_size)) {
                request.response_date->observed_at_ms = monotonic_ms();
            }
            head_received = true;
        } else if (type == kProxyResponseData) {
            if (!head_received || frame_size <= kProxyDataHeaderBytes ||
                read_u32(frame.data() + 10) != response_bytes) {
                result = agent::Error::malformed_response;
                break;
            }
            const std::size_t chunk = frame_size - kProxyDataHeaderBytes;
            if (!budget.accept(chunk)) {
                result = agent::Error::response_too_large;
                break;
            }
            result = sink.write(
                frame.data() + kProxyDataHeaderBytes, chunk);
            if (result == agent::Error::none) {
                body_progress.observe_body_bytes(monotonic_ms());
            }
            response_bytes += chunk;
        } else if (type == kProxyResponseEnd) {
            if (!head_received || frame_size != 14 ||
                read_u32(frame.data() + 10) != response_bytes) {
                result = agent::Error::malformed_response;
                break;
            }
            result = validate_response_completion(
                content_length, response_bytes, true);
            if (result == agent::Error::none) {
                result = sink.finish();
            }
            break;
        } else if (type == kProxyResponseError) {
            if (frame_size != 11) {
                result = agent::Error::malformed_response;
            } else if (frame[10] == 1) {
                result = agent::Error::total_timeout;
            } else if (frame[10] == 2) {
                result = agent::Error::response_too_large;
            } else {
                result = agent::Error::disconnected;
            }
            break;
        } else {
            result = agent::Error::malformed_response;
            break;
        }
    }
    std::fill(frame.begin(), frame.end(), 0);
    if (output_started && result != agent::Error::none) {
        sink.abort();
    }
    ESP_LOGI(
        "http_transport", "BLE proxy byte counts: request=%u response=%u",
        static_cast<unsigned>(body_size),
        static_cast<unsigned>(response_bytes));
    return cancellation.cancelled() ? agent::Error::cancelled : result;
}

}  // namespace

HttpTransport::~HttpTransport() { reset_session(); }

bool HttpTransport::proxy_available() const {
    return ble_provisioning::http_proxy_available();
}

esp_err_t HttpTransport::capture_header(esp_http_client_event_t *event) {
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
    } else if (text_equal_fold(event->header_key, "Date")) {
        if (headers.date.value.assign(event->header_value)) {
            headers.date.observed_at_ms = monotonic_ms();
        } else {
            headers.date.clear();
        }
    }
    return ESP_OK;
}

bool HttpTransport::prepare_client(
    const HttpRequest &request, const char *url,
    const agent::RequestPolicy &timeouts,
    const ElapsedTimer &total_timer) {
    agent::FixedText<max_http_url_bytes> next_origin;
    if (!extract_origin(url, next_origin)) {
        return false;
    }
    bool reused_client = client_ != nullptr;
    if (client_ != nullptr &&
        !same_https_origin(origin_.c_str(), next_origin.c_str())) {
        close_client();
        reused_client = false;
    }
    response_headers_.clear();
    if (client_ == nullptr) {
        esp_http_client_config_t config{};
        config.url = url;
        config.auth_type = HTTP_AUTH_TYPE_NONE;
        config.user_agent = "ChatESP/0.1";
        config.method = request.method == HttpMethod::get ? HTTP_METHOD_GET
                                                          : HTTP_METHOD_POST;
        config.timeout_ms = phase_timeout(
            total_timer, timeouts.total_timeout_ms,
            timeouts.connect_timeout_ms);
        config.disable_auto_redirect = true;
        config.max_authorization_retries = -1;
        config.event_handler = capture_header;
        config.transport_type = HTTP_TRANSPORT_OVER_SSL;
        config.buffer_size = static_cast<int>(kTransferBufferBytes);
        config.buffer_size_tx = static_cast<int>(kTransferBufferBytes);
        config.user_data = &response_headers_;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        client_ = esp_http_client_init(&config);
        if (client_ == nullptr || !origin_.assign(next_origin.c_str())) {
            close_client();
            return false;
        }
    } else if (
        esp_http_client_set_url(client_, url) != ESP_OK ||
        esp_http_client_set_method(
            client_, request.method == HttpMethod::get ? HTTP_METHOD_GET
                                                       : HTTP_METHOD_POST) !=
            ESP_OK ||
        esp_http_client_set_timeout_ms(
            client_,
            phase_timeout(
                total_timer, timeouts.total_timeout_ms,
                timeouts.connect_timeout_ms)) != ESP_OK) {
        close_client();
        return false;
    }
    clear_request_headers();
    ESP_LOGI(
        "http_transport", "HTTPS connection reuse: %u",
        static_cast<unsigned>(reused_client));
    for (std::size_t index = 0; index < request.header_count; ++index) {
        if (esp_http_client_set_header(
                client_, request.headers[index].name,
                request.headers[index].value) != ESP_OK ||
            !applied_header_names_[applied_header_count_].assign(
                request.headers[index].name)) {
            close_client();
            return false;
        }
        ++applied_header_count_;
    }
    return true;
}

void HttpTransport::clear_request_headers() {
    if (client_ != nullptr) {
        for (std::size_t index = 0; index < applied_header_count_; ++index) {
            esp_http_client_delete_header(
                client_, applied_header_names_[index].c_str());
            applied_header_names_[index].clear();
        }
    }
    applied_header_count_ = 0;
}

void HttpTransport::close_client() {
    if (client_ != nullptr) {
        esp_http_client_close(client_);
        esp_http_client_cleanup(client_);
        client_ = nullptr;
    }
    response_headers_.clear();
    origin_.clear();
    for (auto &name : applied_header_names_) {
        name.clear();
    }
    applied_header_count_ = 0;
}

agent::Error HttpTransport::execute(
    const HttpRequest &request, ResponseSink &sink,
    agent::CancellationToken &cancellation) {
    std::lock_guard<std::mutex> request_lock(request_mutex_);
    if (request.response_date != nullptr) {
        request.response_date->clear();
    }
    agent::Error result = validate_request(request);
    if (result != agent::Error::none) {
        return result;
    }
    if (ble_provisioning::http_proxy_available()) {
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            ble_proxy_active_ = true;
        }
        result = execute_ble_proxy(request, sink, cancellation);
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            ble_proxy_active_ = false;
        }
        return result;
    }
    const ElapsedTimer total_timer{monotonic_ms()};
    const std::size_t body_size =
        request.body == nullptr ? 0 : request.body->size();
    std::size_t response_bytes = 0;
    agent::FixedText<max_http_url_bytes> current_url;
    if (!current_url.assign(request.url)) {
        return agent::Error::request_too_large;
    }

    for (std::uint8_t redirect_count = 0;; ++redirect_count) {
        result = check_cancel_or_total(cancellation, total_timer, request.timeouts);
        if (result != agent::Error::none) {
            return result;
        }

        if (!prepare_client(
                request, current_url.c_str(), request.timeouts, total_timer)) {
            return agent::Error::disconnected;
        }
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            active_ = client_;
        }

        if (result == agent::Error::none && request.body != nullptr) {
            result = request.body->reset();
        }
        if (result == agent::Error::none) {
            result = map_esp_error(
                esp_http_client_open(
                    client_,
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
                        client_,
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

        ResponseBodyProgress body_progress{monotonic_ms()};
        if (result == agent::Error::none) {
            esp_http_client_set_timeout_ms(
                client_,
                phase_timeout(
                    total_timer, request.timeouts.total_timeout_ms,
                    body_progress.remaining_timeout_ms(
                        monotonic_ms(), request.timeouts)));
            const std::int64_t fetched = esp_http_client_fetch_headers(client_);
            if (fetched < 0) {
                result = fetched == -ESP_ERR_HTTP_EAGAIN
                             ? agent::Error::first_byte_timeout
                             : agent::Error::disconnected;
            } else if (response_headers_.invalid) {
                result = agent::Error::response_too_large;
            }
        }

        const int status = esp_http_client_get_status_code(client_);
        const std::int64_t content_length =
            esp_http_client_get_content_length(client_);
        const bool redirect =
            result == agent::Error::none && is_redirect_status(status);
        if (redirect) {
            if (!request.allow_image_redirects ||
                redirect_count >= request.max_redirects ||
                !valid_https_url(response_headers_.location.c_str()) ||
                !current_url.assign(response_headers_.location.c_str())) {
                result = agent::Error::malformed_response;
            }
        } else if (result == agent::Error::none) {
            result = validate_response_head(
                status, response_headers_.content_type.c_str(), content_length,
                request.response);
        }

        if (!redirect && result == agent::Error::none) {
            bool output_started = false;
            result = sink.begin(
                status, response_headers_.content_type.c_str(), content_length);
            output_started = result == agent::Error::none;
            ResponseBudget budget{request.response.max_response_bytes};
            while (result == agent::Error::none) {
                result = check_cancel_or_total(
                    cancellation, total_timer, request.timeouts);
                if (result != agent::Error::none) {
                    break;
                }
                const std::uint32_t now_ms = monotonic_ms();
                if (body_progress.expired(now_ms, request.timeouts)) {
                    result = body_progress.timeout_error();
                    break;
                }
                esp_http_client_set_timeout_ms(
                    client_,
                    phase_timeout(
                        total_timer, request.timeouts.total_timeout_ms,
                        body_progress.remaining_timeout_ms(
                            now_ms, request.timeouts)));
                const int read = esp_http_client_read(
                    client_, reinterpret_cast<char *>(buffer.data()),
                    static_cast<int>(buffer.size()));
                if (read == 0) {
                    result = validate_response_completion(
                        content_length, budget.used(),
                        esp_http_client_is_complete_data_received(client_));
                    break;
                }
                if (read < 0) {
                    result = read == -ESP_ERR_HTTP_EAGAIN
                                 ? body_progress.timeout_error()
                                 : agent::Error::disconnected;
                    break;
                }
                if (!budget.accept(static_cast<std::size_t>(read))) {
                    result = agent::Error::response_too_large;
                    break;
                }
                result = sink.write(
                    buffer.data(), static_cast<std::size_t>(read));
                if (result == agent::Error::none) {
                    body_progress.observe_body_bytes(monotonic_ms());
                }
            }
            if (result == agent::Error::none) {
                result = sink.finish();
            }
            response_bytes = budget.used();
            if (output_started && result != agent::Error::none) {
                sink.abort();
            }
        }

        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            active_ = nullptr;
        }

        if (redirect && result == agent::Error::none) {
            close_client();
            continue;
        }
        if (result != agent::Error::none || cancellation.cancelled()) {
            close_client();
        }
        ESP_LOGI(
            "http_transport",
            "HTTPS byte counts: request=%u response=%u",
            static_cast<unsigned>(body_size),
            static_cast<unsigned>(response_bytes));
        if (result == agent::Error::none && request.response_date != nullptr) {
            *request.response_date = response_headers_.date;
        }
        return cancellation.cancelled() ? agent::Error::cancelled : result;
    }
}

void HttpTransport::reset_session() {
    std::lock_guard<std::mutex> request_lock(request_mutex_);
    std::lock_guard<std::mutex> active_lock(active_mutex_);
    close_client();
}

void HttpTransport::cancel_active() {
    std::lock_guard<std::mutex> lock(active_mutex_);
    if (ble_proxy_active_) {
        ble_provisioning::cancel_http_proxy_exchange();
    }
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
