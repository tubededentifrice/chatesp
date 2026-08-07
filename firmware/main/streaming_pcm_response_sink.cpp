#include "streaming_pcm_response_sink.hpp"

#include "chatesp/agent_limits.hpp"
#include "chatesp/openrouter_protocol.hpp"
#include "chatesp/transport_policy.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

namespace chatesp {
namespace cloud {
namespace {

constexpr std::uint32_t kWaitSliceMs = 10;
// Audio start reports progress through the UI. Codec and LVGL calls need much
// more stack than the PCM loop itself.
constexpr std::uint32_t kPlaybackStackBytes = 16 * 1'024;
constexpr UBaseType_t kMinimumPlaybackStackFreeBytes = 4 * 1'024;
constexpr UBaseType_t kPlaybackPriority = 6;
constexpr BaseType_t kPlaybackCore = 1;
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
    stop_and_cleanup();
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
        content_length >
            static_cast<std::int64_t>(agent::Limits::max_tts_pcm_bytes) ||
        (content_length > 0 && content_length % 2 != 0)) {
        return agent::Error::malformed_response;
    }
    if (cancellation_.cancelled()) {
        return agent::Error::cancelled;
    }

    buffer_capacity_ = content_length > 0
        ? static_cast<std::size_t>(content_length)
        : kMaximumBufferBytes;
    storage_ = static_cast<std::uint8_t *>(heap_caps_malloc(
        buffer_capacity_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    playback_chunk_ = static_cast<std::uint8_t *>(heap_caps_malloc(
        kPlaybackChunkBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    mutex_ = xSemaphoreCreateMutex();
    events_ = xEventGroupCreate();
    if (storage_ == nullptr || playback_chunk_ == nullptr ||
        mutex_ == nullptr || events_ == nullptr ||
        !ring_.reset(storage_, buffer_capacity_)) {
        stop_and_cleanup();
        return agent::Error::model_failed;
    }

    total_bytes_ = 0;
    worker_result_ = agent::Error::none;
    producer_done_ = false;
    stop_requested_ = false;
    worker_done_ = false;
    start_policy_.reset();
    session_active_ = true;
    const BaseType_t created = xTaskCreatePinnedToCore(
        playback_task_entry, "tts_playback", kPlaybackStackBytes, this,
        kPlaybackPriority, &task_, kPlaybackCore);
    if (created != pdPASS) {
        task_ = nullptr;
        stop_and_cleanup();
        return agent::Error::model_failed;
    }
    return agent::Error::none;
}

agent::Error StreamingPcmResponseSink::write(
    const std::uint8_t *data, std::size_t size) {
    if (!session_active_ || (size != 0 && data == nullptr)) {
        return agent::Error::invalid_argument;
    }
    if (size > agent::Limits::max_tts_pcm_bytes - total_bytes_) {
        request_stop();
        return agent::Error::limit_exceeded;
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
            total_bytes_ += accepted;
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
            return agent::Error::limit_exceeded;
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
    if (!session_active_) {
        return agent::Error::invalid_argument;
    }
    if (total_bytes_ == 0 || total_bytes_ % 2 != 0) {
        request_stop();
        const agent::Error ignored = wait_for_worker(kWorkerStopTimeoutMs);
        (void)ignored;
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
    stop_and_cleanup();
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
            start_policy_.decision(producer_done);
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
    vTaskDelete(nullptr);
}

void StreamingPcmResponseSink::stop_and_cleanup() {
    if (session_active_ && task_ != nullptr) {
        request_stop();
        if (wait_for_worker(kWorkerStopTimeoutMs) ==
            agent::Error::total_timeout) {
            esp_restart();
        }
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
    task_ = nullptr;
    total_bytes_ = 0;
    buffer_capacity_ = 0;
    start_policy_.reset();
    producer_done_ = false;
    stop_requested_ = false;
    worker_done_ = false;
    session_active_ = false;
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
