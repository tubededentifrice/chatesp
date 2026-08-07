#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "chatesp/agent_interfaces.hpp"
#include "chatesp/byte_ring.hpp"
#include "chatesp/pcm_start_policy.hpp"
#include "http_transport.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace chatesp {
namespace cloud {

// This sink keeps HTTPS and codec writes on different tasks. PCM stays in one
// bounded PSRAM ring and is erased as the playback task consumes it.
class StreamingPcmResponseSink final : public transport::ResponseSink {
public:
    static constexpr std::size_t kMaximumBufferBytes =
        agent::Limits::max_tts_pcm_bytes;
    static_assert(
        runtime::AdaptivePcmStartPolicy::kPrebufferBytes <
            kMaximumBufferBytes,
        "The PCM prebuffer must fit in the response buffer");

    StreamingPcmResponseSink(
        agent::PcmSink &output, agent::CancellationToken &cancellation);
    ~StreamingPcmResponseSink() override;

    StreamingPcmResponseSink(const StreamingPcmResponseSink &) = delete;
    StreamingPcmResponseSink &operator=(
        const StreamingPcmResponseSink &) = delete;

    agent::Error begin(
        int status, const char *content_type,
        std::int64_t content_length) override;
    agent::Error write(
        const std::uint8_t *data, std::size_t size) override;
    agent::Error finish() override;
    void abort() override;

    [[nodiscard]] bool output_started() const {
        return output_started_.load(std::memory_order_acquire);
    }

private:
    static void playback_task_entry(void *context);
    void playback_task();
    void request_stop();
    agent::Error wait_for_worker(std::uint32_t timeout_ms);
    void complete_worker(agent::Error result);
    void stop_and_cleanup();
    bool lock();
    void unlock();

    static constexpr EventBits_t kDataReadyBit = BIT0;
    static constexpr EventBits_t kSpaceReadyBit = BIT1;
    static constexpr EventBits_t kWorkerDoneBit = BIT2;

    agent::PcmSink &output_;
    agent::CancellationToken &cancellation_;
    runtime::ByteRing ring_;
    runtime::AdaptivePcmStartPolicy start_policy_;
    std::uint8_t *storage_ = nullptr;
    std::uint8_t *playback_chunk_ = nullptr;
    std::size_t buffer_capacity_ = 0;
    SemaphoreHandle_t mutex_ = nullptr;
    EventGroupHandle_t events_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::size_t total_bytes_ = 0;
    agent::Error worker_result_ = agent::Error::none;
    bool producer_done_ = false;
    bool stop_requested_ = false;
    bool worker_done_ = false;
    bool session_active_ = false;
    std::atomic<bool> output_started_{false};
};

}  // namespace cloud
}  // namespace chatesp
