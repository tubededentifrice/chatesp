#include "image_frame.hpp"

#include <cstring>

#include "esp_heap_caps.h"

namespace chatesp {
namespace image {

Rgb565Frame::~Rgb565Frame() { reset(); }

Rgb565Frame::Rgb565Frame(Rgb565Frame &&other) noexcept
    : pixels_(other.pixels_) {
    other.pixels_ = nullptr;
}

Rgb565Frame &Rgb565Frame::operator=(Rgb565Frame &&other) noexcept {
    if (this != &other) {
        reset();
        pixels_ = other.pixels_;
        other.pixels_ = nullptr;
    }
    return *this;
}

bool Rgb565Frame::allocate() {
    reset();
    pixels_ = static_cast<std::uint16_t *>(heap_caps_malloc(
        kByteCount, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pixels_ == nullptr) {
        return false;
    }
    clear();
    return true;
}

void Rgb565Frame::clear() {
    if (pixels_ != nullptr) {
        std::memset(pixels_, 0, kByteCount);
    }
}

void Rgb565Frame::reset() {
    if (pixels_ == nullptr) {
        return;
    }
    volatile std::uint16_t *cursor = pixels_;
    for (std::size_t index = 0; index < kPixelCount; ++index) {
        cursor[index] = 0;
    }
    heap_caps_free(pixels_);
    pixels_ = nullptr;
}

}  // namespace image
}  // namespace chatesp
