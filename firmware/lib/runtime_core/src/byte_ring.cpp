#include "chatesp/byte_ring.hpp"

#include <algorithm>
#include <cstring>

namespace chatesp {
namespace runtime {
namespace {

void secure_wipe(std::uint8_t *data, std::size_t size) {
    auto *cursor = static_cast<volatile std::uint8_t *>(data);
    while (size-- != 0) {
        *cursor++ = 0;
    }
}

}  // namespace

bool ByteRing::reset(std::uint8_t *storage, std::size_t capacity) {
    discard();
    storage_ = storage;
    capacity_ = storage == nullptr ? 0 : capacity;
    read_at_ = 0;
    write_at_ = 0;
    size_ = 0;
    return storage_ != nullptr && capacity_ != 0;
}

void ByteRing::discard() {
    if (storage_ != nullptr && capacity_ != 0) {
        secure_wipe(storage_, capacity_);
    }
    read_at_ = 0;
    write_at_ = 0;
    size_ = 0;
}

std::size_t ByteRing::write(
    const std::uint8_t *data, std::size_t size) {
    if ((size != 0 && data == nullptr) || storage_ == nullptr ||
        capacity_ == 0) {
        return 0;
    }
    const std::size_t accepted = std::min(size, free_size());
    const std::size_t first =
        std::min(accepted, capacity_ - write_at_);
    if (first != 0) {
        std::memcpy(storage_ + write_at_, data, first);
    }
    const std::size_t second = accepted - first;
    if (second != 0) {
        std::memcpy(storage_, data + first, second);
    }
    write_at_ = (write_at_ + accepted) % capacity_;
    size_ += accepted;
    return accepted;
}

std::size_t ByteRing::read(
    std::uint8_t *output, std::size_t capacity) {
    if ((capacity != 0 && output == nullptr) || storage_ == nullptr ||
        capacity_ == 0) {
        return 0;
    }
    const std::size_t provided = std::min(capacity, size_);
    const std::size_t first =
        std::min(provided, capacity_ - read_at_);
    if (first != 0) {
        std::memcpy(output, storage_ + read_at_, first);
        secure_wipe(storage_ + read_at_, first);
    }
    const std::size_t second = provided - first;
    if (second != 0) {
        std::memcpy(output + first, storage_, second);
        secure_wipe(storage_, second);
    }
    read_at_ = (read_at_ + provided) % capacity_;
    size_ -= provided;
    return provided;
}

bool ByteRing::unwrite(std::size_t size) {
    if (storage_ == nullptr || capacity_ == 0 || size > size_) {
        return false;
    }
    const std::size_t new_write_at =
        (write_at_ + capacity_ - (size % capacity_)) % capacity_;
    const std::size_t first = std::min(size, capacity_ - new_write_at);
    if (first != 0) {
        secure_wipe(storage_ + new_write_at, first);
    }
    const std::size_t second = size - first;
    if (second != 0) {
        secure_wipe(storage_, second);
    }
    write_at_ = new_write_at;
    size_ -= size;
    return true;
}

}  // namespace runtime
}  // namespace chatesp
