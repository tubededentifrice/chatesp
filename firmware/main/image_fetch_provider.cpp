#include "image_fetch_provider.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "chatesp/image_layout.hpp"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace chatesp::image {
namespace {

constexpr char kJpegContentType[] = "image/jpeg";
constexpr const char *kAcceptedContentTypes[] = {kJpegContentType};
constexpr std::uint32_t kRetryDelayMs = 250;

std::uint32_t monotonic_ms() {
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1'000ULL);
}

bool retry_delay(agent::CancellationToken &cancellation) {
    constexpr std::uint32_t step_ms = 10;
    for (std::uint32_t elapsed = 0; elapsed < kRetryDelayMs;
         elapsed += step_ms) {
        if (cancellation.cancelled()) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }
    return !cancellation.cancelled();
}

bool is_start_of_frame(std::uint8_t marker) {
    return (marker >= 0xc0 && marker <= 0xc3) ||
        (marker >= 0xc5 && marker <= 0xc7) ||
        (marker >= 0xc9 && marker <= 0xcb) ||
        (marker >= 0xcd && marker <= 0xcf);
}

class BaselineJpegHeader {
public:
    agent::Error feed(
        const std::uint8_t *data,
        std::size_t size,
        std::uint32_t maximum_dimension) {
        if ((size != 0 && data == nullptr) || maximum_dimension == 0) {
            return agent::Error::invalid_argument;
        }
        for (std::size_t index = 0; index < size && !complete_; ++index) {
            const agent::Error error = consume(data[index], maximum_dimension);
            if (error != agent::Error::none) {
                return error;
            }
        }
        return agent::Error::none;
    }

    agent::Error finish() const {
        return complete_ ? agent::Error::none
                         : agent::Error::malformed_response;
    }

    [[nodiscard]] std::uint32_t width() const { return width_; }
    [[nodiscard]] std::uint32_t height() const { return height_; }

private:
    enum class State : std::uint8_t {
        soi_prefix,
        soi_code,
        marker_prefix,
        marker_code,
        length_high,
        length_low,
        payload,
    };

    agent::Error consume(
        std::uint8_t value, std::uint32_t maximum_dimension) {
        switch (state_) {
            case State::soi_prefix:
                if (value != 0xff) {
                    return agent::Error::malformed_response;
                }
                state_ = State::soi_code;
                return agent::Error::none;
            case State::soi_code:
                if (value != 0xd8) {
                    return agent::Error::malformed_response;
                }
                state_ = State::marker_prefix;
                return agent::Error::none;
            case State::marker_prefix:
                if (value != 0xff) {
                    return agent::Error::malformed_response;
                }
                state_ = State::marker_code;
                return agent::Error::none;
            case State::marker_code:
                return consume_marker(value);
            case State::length_high:
                segment_length_ = static_cast<std::uint16_t>(value) << 8U;
                state_ = State::length_low;
                return agent::Error::none;
            case State::length_low:
                segment_length_ |= value;
                if (segment_length_ < 2) {
                    return agent::Error::malformed_response;
                }
                segment_remaining_ = segment_length_ - 2U;
                sof_bytes_seen_ = 0;
                state_ = State::payload;
                if (segment_remaining_ == 0) {
                    state_ = State::marker_prefix;
                }
                return agent::Error::none;
            case State::payload:
                return consume_payload(value, maximum_dimension);
        }
        return agent::Error::malformed_response;
    }

    agent::Error consume_marker(std::uint8_t marker) {
        if (marker == 0xff) {
            return agent::Error::none;
        }
        if (marker == 0x00 || marker == 0xd8 || marker == 0xd9 ||
            marker == 0xda || (marker >= 0xd0 && marker <= 0xd7)) {
            return agent::Error::malformed_response;
        }
        if (is_start_of_frame(marker) && marker != 0xc0) {
            return agent::Error::unsupported_media;
        }
        if (marker == 0x01) {
            state_ = State::marker_prefix;
            return agent::Error::none;
        }
        segment_marker_ = marker;
        state_ = State::length_high;
        return agent::Error::none;
    }

    agent::Error consume_payload(
        std::uint8_t value, std::uint32_t maximum_dimension) {
        if (segment_marker_ == 0xc0 && sof_bytes_seen_ < sizeof(sof_bytes_)) {
            sof_bytes_[sof_bytes_seen_++] = value;
        }
        if (segment_remaining_ == 0) {
            return agent::Error::malformed_response;
        }
        --segment_remaining_;
        if (segment_remaining_ != 0) {
            return agent::Error::none;
        }
        state_ = State::marker_prefix;
        if (segment_marker_ != 0xc0) {
            return agent::Error::none;
        }
        if (sof_bytes_seen_ != sizeof(sof_bytes_)) {
            return agent::Error::malformed_response;
        }
        const std::uint8_t components = sof_bytes_[5];
        const std::uint32_t expected_length = 8U + 3U * components;
        if (sof_bytes_[0] != 8 || (components != 1 && components != 3) ||
            segment_length_ != expected_length) {
            return agent::Error::unsupported_media;
        }
        height_ = static_cast<std::uint32_t>(sof_bytes_[1]) << 8U |
            sof_bytes_[2];
        width_ = static_cast<std::uint32_t>(sof_bytes_[3]) << 8U |
            sof_bytes_[4];
        if (width_ == 0 || height_ == 0) {
            return agent::Error::malformed_response;
        }
        if (width_ > maximum_dimension || height_ > maximum_dimension) {
            return agent::Error::limit_exceeded;
        }
        complete_ = true;
        return agent::Error::none;
    }

    State state_ = State::soi_prefix;
    std::uint8_t segment_marker_ = 0;
    std::uint16_t segment_length_ = 0;
    std::uint16_t segment_remaining_ = 0;
    std::uint8_t sof_bytes_[6]{};
    std::size_t sof_bytes_seen_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    bool complete_ = false;
};

class ImageResponseSink final : public transport::ResponseSink {
public:
    ImageResponseSink(
        agent::ByteSink &sink,
        agent::CancellationToken &cancellation,
        std::size_t maximum_bytes,
        std::uint32_t maximum_dimension)
        : sink_(sink),
          cancellation_(cancellation),
          maximum_bytes_(maximum_bytes),
          maximum_dimension_(maximum_dimension) {}

    agent::Error begin(
        int status,
        const char *content_type,
        std::int64_t content_length) override {
        received_bytes_ = 0;
        expected_bytes_ = 0;
        header_ = {};
        sink_started_ = false;
        if (cancellation_.cancelled()) {
            return agent::Error::cancelled;
        }
        const agent::Error status_error = transport::map_http_status(status);
        if (status_error != agent::Error::none) {
            return status_error;
        }
        if (content_type == nullptr ||
            std::strcmp(content_type, kJpegContentType) != 0) {
            return agent::Error::unsupported_media;
        }
        if (content_length <= 0 ||
            static_cast<std::uint64_t>(content_length) > maximum_bytes_ ||
            static_cast<std::uint64_t>(content_length) >
                std::numeric_limits<std::size_t>::max()) {
            return agent::Error::limit_exceeded;
        }
        expected_bytes_ = static_cast<std::size_t>(content_length);
        const agent::ImageMetadata metadata{
            agent::ImageMediaType::jpeg,
            expected_bytes_,
            0,
            0,
        };
        const agent::Error error = sink_.begin(metadata);
        sink_started_ = error == agent::Error::none;
        ever_started_ = ever_started_ || sink_started_;
        return error;
    }

    agent::Error write(
        const std::uint8_t *data, std::size_t size) override {
        if (!sink_started_ || (size != 0 && data == nullptr)) {
            return agent::Error::invalid_argument;
        }
        if (cancellation_.cancelled()) {
            return agent::Error::cancelled;
        }
        if (size > expected_bytes_ - received_bytes_) {
            return agent::Error::limit_exceeded;
        }
        agent::Error error = header_.feed(data, size, maximum_dimension_);
        if (error == agent::Error::none) {
            error = sink_.write(data, size);
        }
        if (error == agent::Error::none) {
            received_bytes_ += size;
        }
        return error;
    }

    agent::Error finish() override {
        if (!sink_started_ || received_bytes_ != expected_bytes_) {
            return agent::Error::malformed_response;
        }
        if (cancellation_.cancelled()) {
            return agent::Error::cancelled;
        }
        const agent::Error header_error = header_.finish();
        return header_error == agent::Error::none ? sink_.finish()
                                                   : header_error;
    }

    void abort() override {
        if (sink_started_) {
            sink_.abort();
            sink_started_ = false;
        }
    }

    [[nodiscard]] bool output_started() const { return ever_started_; }

private:
    agent::ByteSink &sink_;
    agent::CancellationToken &cancellation_;
    std::size_t maximum_bytes_ = 0;
    std::uint32_t maximum_dimension_ = 0;
    BaselineJpegHeader header_;
    std::size_t expected_bytes_ = 0;
    std::size_t received_bytes_ = 0;
    bool sink_started_ = false;
    bool ever_started_ = false;
};

}  // namespace

agent::Error HttpImageFetchProvider::fetch(
    const agent::ImageFetchRequest &request,
    agent::ByteSink &sink,
    agent::CancellationToken &cancellation) {
    agent::Error error = agent::validate_image_fetch_request(request);
    if (error != agent::Error::none) {
        return error;
    }
    if (!is_trusted_brave_thumbnail_url(request.url) ||
        request.max_redirects != 0) {
        return agent::Error::invalid_argument;
    }
    if (cancellation.cancelled()) {
        return agent::Error::cancelled;
    }

    transport::HttpRequest http_request;
    http_request.method = transport::HttpMethod::get;
    http_request.url = request.url;
    http_request.header_count = 0;
    http_request.body = nullptr;
    http_request.max_request_bytes = 1;
    http_request.response = {
        kAcceptedContentTypes,
        1,
        request.max_bytes,
    };
    http_request.timeouts = request.policy;
    http_request.allow_image_redirects = false;
    http_request.max_redirects = 0;

    const std::uint32_t started_ms = monotonic_ms();
    for (std::uint8_t completed = 1;; ++completed) {
        const std::uint32_t elapsed_ms = monotonic_ms() - started_ms;
        if (elapsed_ms >= http_request.timeouts.total_timeout_ms) {
            return agent::Error::total_timeout;
        }
        transport::HttpRequest attempt = http_request;
        attempt.timeouts.total_timeout_ms =
            http_request.timeouts.total_timeout_ms - elapsed_ms;
        ImageResponseSink response(
            sink, cancellation, request.max_bytes, request.max_dimension);
        error = transport_.execute(attempt, response, cancellation);
        if (cancellation.cancelled()) {
            return agent::Error::cancelled;
        }
        if (!agent::retry_allowed(
                error,
                completed,
                response.output_started(),
                request.policy)) {
            return error;
        }
        const std::uint32_t after_attempt_ms = monotonic_ms() - started_ms;
        if (after_attempt_ms >= http_request.timeouts.total_timeout_ms ||
            http_request.timeouts.total_timeout_ms - after_attempt_ms <=
                kRetryDelayMs) {
            return agent::Error::total_timeout;
        }
        if (!retry_delay(cancellation)) {
            return agent::Error::cancelled;
        }
    }
}

}  // namespace chatesp::image
