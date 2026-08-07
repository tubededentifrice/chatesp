#pragma once

#include <array>
#include <cstddef>
#include <cstring>

namespace chatesp {
namespace agent {

template <std::size_t Capacity>
class FixedText {
public:
    constexpr FixedText() = default;
    ~FixedText() { clear(); }

    explicit FixedText(const char *text) { assign(text); }

    bool assign(const char *text) {
        if (text == nullptr) {
            clear();
            return false;
        }
        std::size_t length = 0;
        if (!bounded_length(text, Capacity, length)) {
            clear();
            return false;
        }
        return assign(text, length);
    }

    bool assign(const char *text, std::size_t length) {
        clear();
        return append(text, length);
    }

    bool append(const char *text) {
        if (text == nullptr) {
            return false;
        }
        std::size_t length = 0;
        return bounded_length(text, remaining(), length) && append(text, length);
    }

    bool append(const char *text, std::size_t length) {
        if (text == nullptr || length > remaining()) {
            return false;
        }
        if (length != 0) {
            std::memcpy(data_.data() + size_, text, length);
            size_ += length;
        }
        data_[size_] = '\0';
        return true;
    }

    bool push_back(char value) {
        if (size_ == Capacity) {
            return false;
        }
        data_[size_++] = value;
        data_[size_] = '\0';
        return true;
    }

    void clear() {
        volatile char *cursor = data_.data();
        for (std::size_t index = 0; index < data_.size(); ++index) {
            cursor[index] = '\0';
        }
        size_ = 0;
    }

    [[nodiscard]] const char *c_str() const { return data_.data(); }
    [[nodiscard]] const char *data() const { return data_.data(); }
    [[nodiscard]] char *data() { return data_.data(); }
    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] constexpr std::size_t capacity() const { return Capacity; }
    [[nodiscard]] std::size_t remaining() const { return Capacity - size_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }

    [[nodiscard]] bool equals(const char *text) const {
        std::size_t length = 0;
        return text != nullptr && bounded_length(text, size_, length) &&
               length == size_ && std::memcmp(data_.data(), text, size_) == 0;
    }

private:
    static bool bounded_length(
        const char *text, std::size_t limit, std::size_t &length) {
        length = 0;
        while (length <= limit) {
            if (text[length] == '\0') {
                return true;
            }
            ++length;
        }
        return false;
    }

    std::array<char, Capacity + 1> data_{};
    std::size_t size_ = 0;
};

}  // namespace agent
}  // namespace chatesp
