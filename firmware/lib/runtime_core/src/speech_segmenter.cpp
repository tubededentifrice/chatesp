#include "chatesp/speech_segmenter.hpp"

#include <cstring>

namespace chatesp {
namespace runtime {
namespace {

bool ascii_space(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

bool utf8_continuation(char value) {
    return (static_cast<unsigned char>(value) & 0xC0U) == 0x80U;
}

void secure_wipe(void *data, std::size_t size) {
    auto *cursor = static_cast<volatile unsigned char *>(data);
    while (size-- != 0) {
        *cursor++ = 0;
    }
}

}  // namespace

SpeechSegmenter::~SpeechSegmenter() { reset(); }

bool SpeechSegmenter::update(
    const char *text, std::size_t size, SpeechSegmentSink &sink) {
    if ((size != 0 && text == nullptr) || size < published_size_) {
        return false;
    }
    if (capped()) {
        published_size_ = size;
        return true;
    }
    for (std::size_t index = published_size_; index < size; ++index) {
        if (!consume(text[index], sink)) {
            return false;
        }
    }
    published_size_ = size;
    return true;
}

bool SpeechSegmenter::finish(SpeechSegmentSink &sink) {
    if (capped()) {
        discard_pending();
        return true;
    }
    return pending_size_ == 0 || emit_prefix(pending_size_, sink);
}

void SpeechSegmenter::reset() {
    secure_wipe(pending_.data(), pending_.size());
    pending_size_ = 0;
    published_size_ = 0;
    emitted_bytes_ = 0;
    emitted_segments_ = 0;
}

bool SpeechSegmenter::consume(char value, SpeechSegmentSink &sink) {
    if (capped()) {
        return true;
    }

    if (emitted_segments_ == 0 && pending_size_ != 0 && ascii_space(value) &&
        pending_[pending_size_ - 1] == '.') {
        if (!emit_first_request(sink)) {
            return false;
        }
        if (capped()) {
            return true;
        }
    }
    if (pending_size_ == 0 && ascii_space(value)) {
        return true;
    }
    if (emitted_segments_ == 0 && pending_size_ == kFirstRequestBytes) {
        std::size_t split = pending_size_;
        for (std::size_t index = pending_size_; index > 0; --index) {
            if (ascii_space(pending_[index - 1])) {
                split = index - 1;
                break;
            }
        }
        if (split == pending_size_ && utf8_continuation(value)) {
            while (split > 0 &&
                   utf8_continuation(pending_[split - 1])) {
                --split;
            }
            if (split > 0) {
                --split;
            }
        }
        if (split == 0) {
            return false;
        }
        if (!emit_prefix(split, sink)) {
            return false;
        }
    }
    if (emitted_bytes_ + pending_size_ >= kMaximumSpeechBytes) {
        trim_incomplete_code_point(value);
        return true;
    }
    pending_[pending_size_++] = value;
    pending_[pending_size_] = '\0';

    if (emitted_segments_ == 0 &&
        (value == '?' || value == '!' || value == '\n')) {
        return emit_first_request(sink);
    }
    return true;
}

bool SpeechSegmenter::emit_first_request(SpeechSegmentSink &sink) {
    return emitted_segments_ != 0 || emit_prefix(pending_size_, sink);
}

bool SpeechSegmenter::emit_prefix(
    std::size_t size, SpeechSegmentSink &sink) {
    if (size == 0 || size > pending_size_ || capped()) {
        return size == 0 || capped();
    }
    std::size_t output_size = size;
    while (output_size > 0 && ascii_space(pending_[output_size - 1])) {
        --output_size;
    }
    if (output_size > kMaximumSpeechBytes - emitted_bytes_) {
        output_size = kMaximumSpeechBytes - emitted_bytes_;
        while (output_size > 0 && utf8_continuation(pending_[output_size])) {
            --output_size;
        }
    }
    if (output_size != 0 &&
        !sink.push_speech_segment(pending_.data(), output_size)) {
        return false;
    }
    if (output_size != 0) {
        emitted_bytes_ += output_size;
        ++emitted_segments_;
    }

    const std::size_t remaining = pending_size_ - size;
    if (remaining != 0) {
        std::memmove(pending_.data(), pending_.data() + size, remaining);
    }
    secure_wipe(pending_.data() + remaining, pending_.size() - remaining);
    pending_size_ = remaining;
    while (pending_size_ != 0 && ascii_space(pending_[0])) {
        std::memmove(pending_.data(), pending_.data() + 1, pending_size_ - 1);
        --pending_size_;
        pending_[pending_size_] = '\0';
    }
    if (capped()) {
        discard_pending();
    }
    return true;
}

void SpeechSegmenter::discard_pending() {
    secure_wipe(pending_.data(), pending_.size());
    pending_size_ = 0;
}

void SpeechSegmenter::trim_incomplete_code_point(char excluded_value) {
    if (!utf8_continuation(excluded_value)) {
        return;
    }
    while (pending_size_ != 0 &&
           utf8_continuation(pending_[pending_size_ - 1])) {
        --pending_size_;
    }
    if (pending_size_ != 0) {
        --pending_size_;
    }
    secure_wipe(
        pending_.data() + pending_size_, pending_.size() - pending_size_);
}

bool SpeechSegmenter::capped() const {
    return emitted_segments_ >= kMaximumSegments ||
        emitted_bytes_ >= kMaximumSpeechBytes;
}

}  // namespace runtime
}  // namespace chatesp
