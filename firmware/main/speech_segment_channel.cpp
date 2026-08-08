#include "speech_segment_channel.hpp"

#include <array>

namespace chatesp {
namespace {

constexpr std::uint32_t kWaitSliceMs = 10;
constexpr std::uint32_t kMutexTimeoutMs = 100;

}  // namespace

SpeechSegmentChannel::SpeechSegmentChannel() {
    mutex_ = xSemaphoreCreateMutex();
    events_ = xEventGroupCreate();
}

SpeechSegmentChannel::~SpeechSegmentChannel() {
    cancel();
    if (events_ != nullptr) {
        vEventGroupDelete(events_);
    }
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
    }
}

bool SpeechSegmentChannel::start(
    agent::CancellationToken &cancellation) {
    if (mutex_ == nullptr || events_ == nullptr || !lock()) {
        return false;
    }
    queue_.reset();
    cancellation_ = &cancellation;
    unlock();
    xEventGroupClearBits(events_, kDataBit | kSpaceBit | kStateBit);
    return true;
}

bool SpeechSegmentChannel::push_speech_segment(
    const char *text, std::size_t size) {
    while (true) {
        if (!lock()) {
            return false;
        }
        if (cancellation_ == nullptr || cancellation_->cancelled()) {
            unlock();
            return false;
        }
        const bool accepted = queue_.push_speech_segment(text, size);
        const bool stopped = queue_.cancelled() || queue_.finished();
        const bool full = queue_.full();
        unlock();
        if (accepted) {
            xEventGroupSetBits(events_, kDataBit);
            return true;
        }
        if (stopped || !full) {
            return false;
        }
        xEventGroupWaitBits(
            events_, kSpaceBit | kStateBit, pdTRUE, pdFALSE,
            pdMS_TO_TICKS(kWaitSliceMs));
    }
}

agent::Error SpeechSegmentChannel::next(
    agent::FixedText<agent::Limits::max_tts_segment_bytes> &segment,
    bool &done, agent::CancellationToken &cancellation) {
    segment.clear();
    done = false;
    std::array<char, runtime::SpeechSegmentQueue::kSegmentBytes + 1> text{};
    while (true) {
        if (cancellation.cancelled()) {
            return agent::Error::cancelled;
        }
        if (!lock()) {
            return agent::Error::model_failed;
        }
        std::size_t size = 0;
        const runtime::SpeechQueueResult result =
            queue_.pop(text.data(), text.size() - 1, size);
        unlock();
        if (result == runtime::SpeechQueueResult::ready) {
            const bool assigned = segment.assign(text.data(), size);
            volatile char *cursor = text.data();
            for (std::size_t index = 0; index < text.size(); ++index) {
                cursor[index] = 0;
            }
            xEventGroupSetBits(events_, kSpaceBit);
            return assigned ? agent::Error::none
                            : agent::Error::limit_exceeded;
        }
        if (result == runtime::SpeechQueueResult::finished) {
            done = true;
            return agent::Error::none;
        }
        if (result == runtime::SpeechQueueResult::cancelled) {
            return agent::Error::cancelled;
        }
        if (result != runtime::SpeechQueueResult::empty) {
            return agent::Error::model_failed;
        }
        xEventGroupWaitBits(
            events_, kDataBit | kStateBit, pdTRUE, pdFALSE,
            pdMS_TO_TICKS(kWaitSliceMs));
    }
}

void SpeechSegmentChannel::finish() {
    if (lock()) {
        queue_.finish();
        unlock();
    }
    if (events_ != nullptr) {
        xEventGroupSetBits(events_, kStateBit | kDataBit);
    }
}

void SpeechSegmentChannel::discard_pending_and_finish() {
    if (lock()) {
        queue_.discard_pending_and_finish();
        unlock();
    }
    if (events_ != nullptr) {
        xEventGroupSetBits(events_, kStateBit | kDataBit | kSpaceBit);
    }
}

void SpeechSegmentChannel::cancel() {
    if (lock()) {
        queue_.cancel();
        cancellation_ = nullptr;
        unlock();
    }
    if (events_ != nullptr) {
        xEventGroupSetBits(events_, kStateBit | kDataBit | kSpaceBit);
    }
}

void SpeechSegmentChannel::reset() {
    if (lock()) {
        queue_.reset();
        cancellation_ = nullptr;
        unlock();
    }
}

bool SpeechSegmentChannel::lock() {
    return mutex_ != nullptr &&
        xSemaphoreTake(mutex_, pdMS_TO_TICKS(kMutexTimeoutMs)) == pdTRUE;
}

void SpeechSegmentChannel::unlock() { xSemaphoreGive(mutex_); }

}  // namespace chatesp
