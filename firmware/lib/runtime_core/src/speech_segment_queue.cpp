#include "chatesp/speech_segment_queue.hpp"

#include <cstring>

namespace chatesp {
namespace runtime {

SpeechSegmentQueue::~SpeechSegmentQueue() { reset(); }

bool SpeechSegmentQueue::push_speech_segment(
    const char *text, std::size_t size) {
    if (cancelled_ || finished_ || full() || text == nullptr || size == 0 ||
        size > kSegmentBytes) {
        return false;
    }
    Slot &slot = slots_[write_at_];
    wipe(slot);
    std::memcpy(slot.text.data(), text, size);
    slot.text[size] = '\0';
    slot.size = size;
    write_at_ = (write_at_ + 1) % kCapacity;
    ++count_;
    return true;
}

SpeechQueueResult SpeechSegmentQueue::pop(
    char *output, std::size_t capacity, std::size_t &size) {
    size = 0;
    if (cancelled_) {
        return SpeechQueueResult::cancelled;
    }
    if (empty()) {
        return finished_ ? SpeechQueueResult::finished
                         : SpeechQueueResult::empty;
    }
    Slot &slot = slots_[read_at_];
    if (output == nullptr || capacity < slot.size) {
        return SpeechQueueResult::invalid;
    }
    std::memcpy(output, slot.text.data(), slot.size);
    size = slot.size;
    wipe(slot);
    read_at_ = (read_at_ + 1) % kCapacity;
    --count_;
    return SpeechQueueResult::ready;
}

void SpeechSegmentQueue::finish() { finished_ = true; }

void SpeechSegmentQueue::discard_pending_and_finish() {
    for (auto &slot : slots_) {
        wipe(slot);
    }
    read_at_ = 0;
    write_at_ = 0;
    count_ = 0;
    finished_ = true;
}

void SpeechSegmentQueue::cancel() {
    cancelled_ = true;
    for (auto &slot : slots_) {
        wipe(slot);
    }
    read_at_ = 0;
    write_at_ = 0;
    count_ = 0;
}

void SpeechSegmentQueue::reset() {
    for (auto &slot : slots_) {
        wipe(slot);
    }
    read_at_ = 0;
    write_at_ = 0;
    count_ = 0;
    finished_ = false;
    cancelled_ = false;
}

void SpeechSegmentQueue::wipe(Slot &slot) {
    auto *cursor = reinterpret_cast<volatile unsigned char *>(
        slot.text.data());
    for (std::size_t index = 0; index < slot.text.size(); ++index) {
        cursor[index] = 0;
    }
    slot.size = 0;
}

}  // namespace runtime
}  // namespace chatesp
