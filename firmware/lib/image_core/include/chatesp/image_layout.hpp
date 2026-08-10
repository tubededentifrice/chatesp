#pragma once

#include <cstdint>

namespace chatesp {
namespace image {

constexpr std::uint16_t kDisplayWidth = 368;
constexpr std::uint16_t kDisplayHeight = 448;
constexpr std::uint32_t kMaximumSourceDimension = 2'048;
constexpr std::uint8_t kMaximumJpegScale = 3;

struct Region {
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
};

struct ImageLayout {
    std::uint8_t jpeg_scale = 0;
    std::uint16_t scaled_width = 0;
    std::uint16_t scaled_height = 0;
    Region source_crop{};
    Region destination{};
};

[[nodiscard]] bool plan_jpeg_layout(
    std::uint32_t source_width,
    std::uint32_t source_height,
    std::uint32_t maximum_source_dimension,
    ImageLayout &output);

[[nodiscard]] std::uint16_t rgb888_to_rgb565(
    std::uint8_t red, std::uint8_t green, std::uint8_t blue);
[[nodiscard]] std::uint16_t rom_rgb888_to_rgb565(
    std::uint8_t red, std::uint8_t green, std::uint8_t blue);

// Tiny JPEG rectangles include the right and bottom pixels.
struct InclusiveRect {
    std::uint32_t left = 0;
    std::uint32_t top = 0;
    std::uint32_t right = 0;
    std::uint32_t bottom = 0;
};

enum class ClipResult : std::uint8_t {
    visible,
    outside,
    invalid,
};

[[nodiscard]] std::uint16_t source_x_for_destination(
    const ImageLayout &layout, std::uint16_t destination_x);
[[nodiscard]] std::uint16_t source_y_for_destination(
    const ImageLayout &layout, std::uint16_t destination_y);
[[nodiscard]] ClipResult map_cover_block(
    const ImageLayout &layout,
    const InclusiveRect &block,
    Region &destination);

}  // namespace image
}  // namespace chatesp
