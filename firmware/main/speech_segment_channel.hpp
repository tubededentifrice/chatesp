#pragma once

#include "chatesp/agent_interfaces.hpp"
#include "chatesp/speech_segment_queue.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

namespace chatesp {

static_assert(
    runtime::SpeechSegmentQueue::kSegmentBytes ==
        agent::Limits::max_tts_segment_bytes,
    "The speech queue and provider request limits must match");
static_assert(
    runtime::SpeechSegmentQueue::kCapacity == agent::Limits::max_tts_segments,
    "The speech queue and provider request counts must match");
static_assert(
    runtime::SpeechSegmenter::kFirstRequestBytes ==
        agent::Limits::max_tts_first_request_bytes,
    "The first speech request limits must match");

class SpeechSegmentChannel final
    : public runtime::SpeechSegmentSink,
      public agent::SpeechProvider::SegmentSource {
public:
    SpeechSegmentChannel();
    ~SpeechSegmentChannel() override;

    SpeechSegmentChannel(const SpeechSegmentChannel &) = delete;
    SpeechSegmentChannel &operator=(const SpeechSegmentChannel &) = delete;

    bool start(agent::CancellationToken &cancellation);
    bool push_speech_segment(const char *text, std::size_t size) override;
    agent::Error next(
        agent::FixedText<agent::Limits::max_tts_segment_bytes> &segment,
        bool &done, agent::CancellationToken &cancellation) override;
    void finish();
    void discard_pending_and_finish();
    void cancel();
    void reset();

private:
    bool lock();
    void unlock();

    static constexpr EventBits_t kDataBit = BIT0;
    static constexpr EventBits_t kSpaceBit = BIT1;
    static constexpr EventBits_t kStateBit = BIT2;

    runtime::SpeechSegmentQueue queue_;
    SemaphoreHandle_t mutex_ = nullptr;
    EventGroupHandle_t events_ = nullptr;
    agent::CancellationToken *cancellation_ = nullptr;
};

}  // namespace chatesp
