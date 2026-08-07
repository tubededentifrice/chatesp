#pragma once

#include <cstddef>

namespace chatesp {

class AudioSampleBudget {
public:
    explicit constexpr AudioSampleBudget(std::size_t capacity_samples)
        : capacity_samples_(capacity_samples) {}

    [[nodiscard]] constexpr std::size_t used() const { return used_samples_; }

    [[nodiscard]] constexpr std::size_t remaining() const {
        return capacity_samples_ - used_samples_;
    }

    [[nodiscard]] constexpr bool commit(std::size_t sample_count) {
        if (sample_count > remaining()) {
            return false;
        }
        used_samples_ += sample_count;
        return true;
    }

    constexpr void reset() { used_samples_ = 0; }

private:
    std::size_t capacity_samples_;
    std::size_t used_samples_ = 0;
};

}  // namespace chatesp
