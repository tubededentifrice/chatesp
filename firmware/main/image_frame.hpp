#pragma once

#include <cstddef>
#include <cstdint>

#include "chatesp/image_layout.hpp"

namespace chatesp {
namespace image {

class Rgb565Frame {
public:
    static constexpr std::size_t kPixelCount =
        kDisplayWidth * kDisplayHeight;
    static constexpr std::size_t kByteCount =
        kPixelCount * sizeof(std::uint16_t);

    Rgb565Frame() = default;
    ~Rgb565Frame();

    Rgb565Frame(const Rgb565Frame &) = delete;
    Rgb565Frame &operator=(const Rgb565Frame &) = delete;

    Rgb565Frame(Rgb565Frame &&other) noexcept;
    Rgb565Frame &operator=(Rgb565Frame &&other) noexcept;

    [[nodiscard]] bool allocate();
    void clear();
    void reset();

    [[nodiscard]] std::uint16_t *data() { return pixels_; }
    [[nodiscard]] const std::uint16_t *data() const { return pixels_; }
    [[nodiscard]] std::size_t size_bytes() const {
        return pixels_ == nullptr ? 0 : kByteCount;
    }
    [[nodiscard]] bool available() const { return pixels_ != nullptr; }

private:
    std::uint16_t *pixels_ = nullptr;
};

}  // namespace image
}  // namespace chatesp
