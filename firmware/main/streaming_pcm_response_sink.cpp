#include "streaming_pcm_response_sink.hpp"

#include "chatesp/agent_limits.hpp"
#include "chatesp/openrouter_protocol.hpp"
#include "chatesp/transport_policy.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "resource_telemetry.hpp"
#include "task_config.hpp"

namespace chatesp {
namespace cloud {
namespace {

constexpr std::uint32_t kWaitSliceMs = 10;
// Audio start reports progress through the UI. Codec and LVGL calls need much
// more stack than the PCM loop itself.
constexpr UBaseType_t kMinimumPlaybackStackFreeBytes = 4 * 1'024;
constexpr std::size_t kPlaybackChunkBytes = 2'048;
constexpr std::uint32_t kWorkerStopTimeoutMs = 1'000;
constexpr std::uint32_t kWorkerDrainTimeoutMs = 50'000;
constexpr std::uint32_t kMutexTimeoutMs = 100;
constexpr char kTag[] = "tts_stream";

void secure_wipe(void *data, std::size_t size) {
    auto *cursor = static_cast<volatile std::uint8_t *>(data);
    while (size-- != 0) {
        *cursor++ = 0;
    }
}

std::uint32_t monotonic_ms() {
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1'000ULL);
}

}  // namespace

StreamingPcmResponseSink::StreamingPcmResponseSink(
    agent::PcmSink &output, agent::CancellationToken &cancellation)
    : output_(output), cancellation_(cancellation) {}

StreamingPcmResponseSink::~StreamingPcmResponseSink() {
    stop_and_cleanup();
}

agent::Error StreamingPcmResponseSink::begin(
    int status, const char *content_type, std::int64_t content_length) {
    const agent::Error status_error = transport::map_http_status(status);
    if (status_error != agent::Error::none) {
        return status_error;
    }
    const agent::Error media_error =
        agent::validate_openrouter_pcm_content_type(content_type);
    if (media_error != agent::Error::none) {
        return media_error;
    }
    if (content_length == 0 ||
        (content_length > 0 && content_length % 2 != 0)) {
        return agent::Error::malformed_response;
    }
    if (content_length >
        static_cast<std::int64_t>(agent::Limits::max_tts_pcm_bytes)) {
        return agent::Error::response_too_large;
    }
    if (cancellation_.cancelled()) {
        return agent::Error::cancelled;
    }

    if (!session_active_) {
        const agent::Error start_error = start_session();
        if (start_error != agent::Error::none) {
            return start_error;
        }
    }
    if (!response_prepared_ || response_active_ || producer_done_) {
        return agent::Error::limit_exceeded;
    }
    if (content_length > 0 &&
        static_cast<std::size_t>(content_length) >
            kMaximumBufferBytes - total_bytes_) {
        return agent::Error::response_too_large;
    }
    response_active_ = true;
    response_bytes_ = 0;
    response_length_ = content_length;
    return agent::Error::none;
}

agent::Error StreamingPcmResponseSink::start_session() {
    stop_and_cleanup();
    buffer_capacity_ = kMaximumBufferBytes;
    storage_ = static_cast<std::uint8_t *>(heap_caps_malloc(
        buffer_capacity_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    playback_chunk_ = static_cast<std::uint8_t *>(heap_caps_malloc(
        kPlaybackChunkBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    mutex_ = xSemaphoreCreateMutex();
    events_ = xEventGroupCreate();
    if (storage_ == nullptr || playback_chunk_ == nullptr ||
        mutex_ == nullptr || events_ == nullptr ||
        !ring_.reset(storage_, buffer_capacity_)) {
        ESP_LOGE(kTag, "PCM playback resources are unavailable");
        stop_and_cleanup();
        return agent::Error::model_failed;
    }

    total_bytes_ = 0;
    worker_result_ = agent::Error::none;
    producer_done_ = false;
    stop_requested_ = false;
    worker_done_ = false;
    start_policy_.reset();
    response_bytes_ = 0;
    response_length_ = -1;
    response_active_ = false;
    first_response_complete_ = false;
    response_prepared_ = false;
    played_bytes_.store(0, std::memory_order_release);
    response_start_offset_.store(0, std::memory_order_release);
    session_active_ = true;
    const BaseType_t created = task_config::create(
        playback_task_entry, task_config::tts_playback, this, &task_);
    if (created != pdPASS) {
        ESP_LOGE(kTag, "PCM playback worker could not start");
        task_ = nullptr;
        stop_and_cleanup();
        return agent::Error::model_failed;
    }
    return agent::Error::none;
}

agent::Error StreamingPcmResponseSink::write(
    const std::uint8_t *data, std::size_t size) {
    if (!session_active_ || !response_active_ ||
        (size != 0 && data == nullptr)) {
        return agent::Error::invalid_argument;
    }
    if (size > agent::Limits::max_tts_pcm_bytes - total_bytes_) {
        request_stop();
        return agent::Error::response_too_large;
    }

    std::size_t offset = 0;
    while (offset < size) {
        if (cancellation_.cancelled()) {
            request_stop();
            return agent::Error::cancelled;
        }
        if (!lock()) {
            request_stop();
            return agent::Error::model_failed;
        }
        if (worker_done_) {
            const agent::Error result = worker_result_;
            unlock();
            return result == agent::Error::none
                       ? agent::Error::model_failed
                       : result;
        }
        const std::size_t accepted =
            ring_.write(data + offset, size - offset);
        bool policy_decided = false;
        bool streams_early = false;
        if (accepted != 0) {
            if (total_bytes_ == 0) {
                ESP_LOGI(kTag, "Phase event: first PCM byte");
            }
            total_bytes_ += accepted;
            response_bytes_ += accepted;
            const bool was_decided = start_policy_.decided();
            start_policy_.observe(total_bytes_, monotonic_ms());
            policy_decided = !was_decided && start_policy_.decided();
            streams_early = start_policy_.streams_early();
        }
        unlock();
        if (accepted != 0) {
            offset += accepted;
            if (policy_decided) {
                ESP_LOGI(
                    kTag, "PCM ingress mode: %s",
                    streams_early ? "streaming" : "buffered");
            }
            xEventGroupSetBits(events_, kDataReadyBit);
            continue;
        }
        if (total_bytes_ >= buffer_capacity_) {
            request_stop();
            return agent::Error::response_too_large;
        }
        const EventBits_t bits = xEventGroupWaitBits(
            events_, kSpaceReadyBit | kWorkerDoneBit, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(kWaitSliceMs));
        if ((bits & kSpaceReadyBit) != 0) {
            xEventGroupClearBits(events_, kSpaceReadyBit);
        }
    }
    return agent::Error::none;
}

agent::Error StreamingPcmResponseSink::finish() {
    if (!session_active_ || !response_active_) {
        return agent::Error::invalid_argument;
    }
    if (response_bytes_ == 0 || response_bytes_ % 2 != 0 ||
        (response_length_ >= 0 &&
         response_bytes_ != static_cast<std::size_t>(response_length_))) {
        return agent::Error::malformed_response;
    }
    if (!lock()) {
        request_stop();
        return agent::Error::model_failed;
    }
    response_active_ = false;
    response_prepared_ = false;
    first_response_complete_ = true;
    unlock();
    xEventGroupSetBits(events_, kDataReadyBit);
    return agent::Error::none;
}

agent::Error StreamingPcmResponseSink::prepare_response() {
    if (response_active_ || producer_done_ || cancellation_.cancelled()) {
        return cancellation_.cancelled() ? agent::Error::cancelled
                                         : agent::Error::invalid_argument;
    }
    if (!session_active_) {
        const agent::Error start_error = start_session();
        if (start_error != agent::Error::none) {
            return start_error;
        }
    }
    if (!lock()) {
        return agent::Error::model_failed;
    }
    if (worker_done_) {
        const agent::Error result = worker_result_ == agent::Error::none
            ? agent::Error::model_failed
            : worker_result_;
        unlock();
        return result;
    }
    const std::size_t response_start =
        played_bytes_.load(std::memory_order_acquire) + ring_.size();
    response_start_offset_.store(response_start, std::memory_order_release);
    response_prepared_ = true;
    unlock();
    return agent::Error::none;
}

bool StreamingPcmResponseSink::current_segment_started() const {
    return played_bytes_.load(std::memory_order_acquire) >
        response_start_offset_.load(std::memory_order_acquire);
}

agent::Error StreamingPcmResponseSink::finish_sequence() {
    if (!session_active_ || response_active_ || !first_response_complete_) {
        stop_and_cleanup();
        return agent::Error::malformed_response;
    }
    if (!lock()) {
        request_stop();
        stop_and_cleanup();
        return agent::Error::model_failed;
    }
    producer_done_ = true;
    unlock();
    xEventGroupSetBits(events_, kDataReadyBit);
    agent::Error result = wait_for_worker(kWorkerDrainTimeoutMs);
    if (result == agent::Error::total_timeout) {
        request_stop();
        result = wait_for_worker(kWorkerStopTimeoutMs);
    }
    if (result == agent::Error::total_timeout) {
        esp_restart();
    }
    stop_and_cleanup();
    return result;
}

void StreamingPcmResponseSink::abort() {
    if (!session_active_) {
        return;
    }
    // A retry can replace a response only before playback starts. Once PCM is
    // audible, stop the sequence so the listener cannot hear duplicate data.
    if (response_active_ && !current_segment_started() && lock()) {
        const bool removed = ring_.unwrite(response_bytes_);
        if (removed) {
            total_bytes_ -= response_bytes_;
            response_bytes_ = 0;
            response_length_ = -1;
            response_active_ = false;
            if (!first_response_complete_) {
                start_policy_.reset();
            }
        }
        unlock();
        if (removed) {
            return;
        }
    }
    request_stop();
}

void StreamingPcmResponseSink::playback_task_entry(void *context) {
    static_cast<StreamingPcmResponseSink *>(context)->playback_task();
}

void StreamingPcmResponseSink::playback_task() {
    bool output_open = false;
    while (true) {
        if (!lock()) {
            if (output_open) {
                output_.finish();
            }
            complete_worker(agent::Error::model_failed);
            return;
        }
        const std::size_t available = ring_.size();
        const bool stop = stop_requested_ || cancellation_.cancelled();
        const bool producer_done = producer_done_;
        const runtime::PcmStartDecision start_decision =
            start_policy_.decision(first_response_complete_);
        const bool ready_to_start = !output_open &&
            available != 0 &&
            start_decision != runtime::PcmStartDecision::wait;
        unlock();

        if (stop) {
            if (output_open) {
                output_.finish();
            }
            complete_worker(agent::Error::cancelled);
            return;
        }
        if (ready_to_start) {
            ESP_LOGI(
                kTag, "Playback stack free before output start: %u bytes",
                static_cast<unsigned>(
                    uxTaskGetStackHighWaterMark(nullptr)));
            const agent::Error begin_error = output_.begin(24'000, 1, 16);
            if (begin_error != agent::Error::none) {
                complete_worker(begin_error);
                return;
            }
            output_open = true;
            output_started_.store(true, std::memory_order_release);
            const UBaseType_t stack_free =
                uxTaskGetStackHighWaterMark(nullptr);
            ESP_LOGI(
                kTag, "Playback stack minimum free bytes: %u",
                static_cast<unsigned>(stack_free));
            if (stack_free < kMinimumPlaybackStackFreeBytes) {
                ESP_LOGE(kTag, "Playback stack safety margin is too small");
                output_.cancel();
                output_.finish();
                complete_worker(agent::Error::model_failed);
                return;
            }
            continue;
        }

        std::size_t read_size = 0;
        if (output_open && available != 0) {
            if (!lock()) {
                output_.finish();
                complete_worker(agent::Error::model_failed);
                return;
            }
            read_size = ring_.read(
                playback_chunk_, kPlaybackChunkBytes);
            unlock();
            if (read_size != 0) {
                played_bytes_.fetch_add(
                    read_size, std::memory_order_acq_rel);
                xEventGroupSetBits(events_, kSpaceReadyBit);
                const agent::Error write_error =
                    output_.write(playback_chunk_, read_size);
                secure_wipe(playback_chunk_, kPlaybackChunkBytes);
                if (write_error != agent::Error::none) {
                    output_.finish();
                    complete_worker(write_error);
                    return;
                }
                continue;
            }
        }
        if (producer_done && available == 0) {
            const agent::Error result = output_open
                ? output_.finish()
                : agent::Error::malformed_response;
            complete_worker(result);
            return;
        }
        xEventGroupWaitBits(
            events_, kDataReadyBit, pdTRUE, pdFALSE,
            pdMS_TO_TICKS(kWaitSliceMs));
    }
}

void StreamingPcmResponseSink::request_stop() {
    if (!session_active_ || mutex_ == nullptr || events_ == nullptr) {
        return;
    }
    output_.cancel();
    if (lock()) {
        stop_requested_ = true;
        unlock();
    }
    xEventGroupSetBits(events_, kDataReadyBit | kSpaceReadyBit);
}

agent::Error StreamingPcmResponseSink::wait_for_worker(
    std::uint32_t timeout_ms) {
    const std::uint32_t started_ms = monotonic_ms();
    while (session_active_) {
        if (cancellation_.cancelled()) {
            request_stop();
        }
        if (lock()) {
            const bool done = worker_done_;
            unlock();
            if (done) {
                break;
            }
        }
        if (monotonic_ms() - started_ms >= timeout_ms) {
            return agent::Error::total_timeout;
        }
        const EventBits_t bits = xEventGroupWaitBits(
            events_, kWorkerDoneBit, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(kWaitSliceMs));
        if ((bits & kWorkerDoneBit) != 0) {
            break;
        }
    }
    if (!lock()) {
        return agent::Error::model_failed;
    }
    const agent::Error result = worker_result_;
    unlock();
    return cancellation_.cancelled() ? agent::Error::cancelled : result;
}

void StreamingPcmResponseSink::complete_worker(agent::Error result) {
    if (lock()) {
        worker_result_ = result;
        worker_done_ = true;
        unlock();
    }
    xEventGroupSetBits(events_, kWorkerDoneBit | kSpaceReadyBit);
    resource_telemetry::record_task_watermark(
        task_config::TaskId::tts_playback);
    // The owner frees the PSRAM-backed task stack with the matching IDF API.
    vTaskSuspend(nullptr);
}

void StreamingPcmResponseSink::stop_and_cleanup() {
    if (session_active_ && task_ != nullptr) {
        request_stop();
        if (wait_for_worker(kWorkerStopTimeoutMs) ==
            agent::Error::total_timeout) {
            esp_restart();
        }
    }
    if (task_ != nullptr) {
        vTaskDeleteWithCaps(task_);
        task_ = nullptr;
    }
    if (mutex_ != nullptr && lock()) {
        ring_.reset(nullptr, 0);
        unlock();
    }
    if (storage_ != nullptr) {
        heap_caps_free(storage_);
        storage_ = nullptr;
    }
    if (playback_chunk_ != nullptr) {
        secure_wipe(playback_chunk_, kPlaybackChunkBytes);
        heap_caps_free(playback_chunk_);
        playback_chunk_ = nullptr;
    }
    if (events_ != nullptr) {
        vEventGroupDelete(events_);
        events_ = nullptr;
    }
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
    }
    total_bytes_ = 0;
    response_bytes_ = 0;
    response_length_ = -1;
    buffer_capacity_ = 0;
    start_policy_.reset();
    producer_done_ = false;
    stop_requested_ = false;
    worker_done_ = false;
    session_active_ = false;
    response_active_ = false;
    response_prepared_ = false;
    first_response_complete_ = false;
    output_started_.store(false, std::memory_order_release);
    played_bytes_.store(0, std::memory_order_release);
    response_start_offset_.store(0, std::memory_order_release);
}

bool StreamingPcmResponseSink::lock() {
    return mutex_ != nullptr &&
        xSemaphoreTake(mutex_, pdMS_TO_TICKS(kMutexTimeoutMs)) == pdTRUE;
}

void StreamingPcmResponseSink::unlock() {
    xSemaphoreGive(mutex_);
}

}  // namespace cloud
}  // namespace chatesp
