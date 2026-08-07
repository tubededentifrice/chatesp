#pragma once

#include <cstddef>
#include <cstdint>

namespace chatesp {
namespace runtime {

// This bounded ring does not own its storage. The caller must serialize access.
class ByteRing {
public:
    bool reset(std::uint8_t *storage, std::size_t capacity);
    void discard();

    [[nodiscard]] std::size_t write(
        const std::uint8_t *data, std::size_t size);
    [[nodiscard]] std::size_t read(
        std::uint8_t *output, std::size_t capacity);

    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] std::size_t free_size() const { return capacity_ - size_; }
    [[nodiscard]] std::size_t capacity() const { return capacity_; }

private:
    std::uint8_t *storage_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t read_at_ = 0;
    std::size_t write_at_ = 0;
    std::size_t size_ = 0;
};

}  // namespace runtime
}  // namespace chatesp
