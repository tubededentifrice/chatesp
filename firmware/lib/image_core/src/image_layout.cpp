#include "chatesp/image_layout.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace chatesp {
namespace image {
namespace {

std::uint32_t scaled_dimension(
    std::uint32_t dimension, std::uint8_t scale) {
    return dimension >> scale;
}

bool valid_region(
    const Region &region,
    std::uint32_t maximum_width,
    std::uint32_t maximum_height) {
    if (region.width == 0 || region.height == 0 ||
        region.x > maximum_width || region.y > maximum_height) {
        return false;
    }
    return region.width <= maximum_width - region.x &&
           region.height <= maximum_height - region.y;
}

}  // namespace

bool plan_jpeg_layout(
    std::uint32_t source_width,
    std::uint32_t source_height,
    std::uint32_t maximum_source_dimension,
    ImageLayout &output) {
    output = {};
    if (source_width == 0 || source_height == 0 ||
        maximum_source_dimension == 0 ||
        maximum_source_dimension > kMaximumSourceDimension ||
        source_width > maximum_source_dimension ||
        source_height > maximum_source_dimension) {
        return false;
    }

    std::uint8_t selected_scale = 0;
    for (std::uint8_t scale = 1; scale <= kMaximumJpegScale; ++scale) {
        const std::uint32_t width = scaled_dimension(source_width, scale);
        const std::uint32_t height = scaled_dimension(source_height, scale);
        if (width < kDisplayWidth || height < kDisplayHeight) {
            break;
        }
        selected_scale = scale;
    }

    const std::uint32_t scaled_width =
        scaled_dimension(source_width, selected_scale);
    const std::uint32_t scaled_height =
        scaled_dimension(source_height, selected_scale);
    if (scaled_width == 0 || scaled_height == 0 ||
        scaled_width > std::numeric_limits<std::uint16_t>::max() ||
        scaled_height > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }

    std::uint32_t crop_width = scaled_width;
    std::uint32_t crop_height = scaled_height;
    if (scaled_width * kDisplayHeight > scaled_height * kDisplayWidth) {
        crop_width = std::max<std::uint32_t>(
            1U, scaled_height * kDisplayWidth / kDisplayHeight);
    } else {
        crop_height = std::max<std::uint32_t>(
            1U, scaled_width * kDisplayHeight / kDisplayWidth);
    }
    output.jpeg_scale = selected_scale;
    output.scaled_width = static_cast<std::uint16_t>(scaled_width);
    output.scaled_height = static_cast<std::uint16_t>(scaled_height);
    output.source_crop = {
        static_cast<std::uint16_t>((scaled_width - crop_width) / 2),
        static_cast<std::uint16_t>((scaled_height - crop_height) / 2),
        static_cast<std::uint16_t>(crop_width),
        static_cast<std::uint16_t>(crop_height),
    };
    output.destination = {0, 0, kDisplayWidth, kDisplayHeight};
    return true;
}

std::uint16_t rgb888_to_rgb565(
    std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(red & 0xF8U) << 8U) |
        (static_cast<std::uint16_t>(green & 0xFCU) << 3U) |
        (static_cast<std::uint16_t>(blue) >> 3U));
}

std::uint16_t rom_rgb888_to_rgb565(
    std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    return rgb888_to_rgb565(red, green, blue);
}

bool is_trusted_brave_thumbnail_url(const char *url) {
    constexpr char origin[] = "https://imgs.search.brave.com";
    constexpr std::size_t origin_size = sizeof(origin) - 1U;
    return url != nullptr && std::strncmp(url, origin, origin_size) == 0 &&
        url[origin_size] == '/';
}

namespace {

std::uint16_t map_destination_coordinate(
    std::uint16_t destination,
    std::uint16_t destination_extent,
    std::uint16_t source_start,
    std::uint16_t source_extent) {
    const std::uint64_t numerator =
        (static_cast<std::uint64_t>(destination) * 2U + 1U) * source_extent;
    std::uint32_t offset = static_cast<std::uint32_t>(
        numerator / (static_cast<std::uint64_t>(destination_extent) * 2U));
    if (offset >= source_extent) {
        offset = source_extent - 1U;
    }
    return static_cast<std::uint16_t>(source_start + offset);
}

bool valid_cover_layout(const ImageLayout &layout) {
    return layout.scaled_width != 0 && layout.scaled_height != 0 &&
        layout.jpeg_scale <= kMaximumJpegScale &&
        valid_region(
            layout.source_crop, layout.scaled_width, layout.scaled_height) &&
        layout.destination.x == 0 && layout.destination.y == 0 &&
        layout.destination.width == kDisplayWidth &&
        layout.destination.height == kDisplayHeight;
}

}  // namespace

std::uint16_t source_x_for_destination(
    const ImageLayout &layout, std::uint16_t destination_x) {
    return map_destination_coordinate(
        destination_x,
        kDisplayWidth,
        layout.source_crop.x,
        layout.source_crop.width);
}

std::uint16_t source_y_for_destination(
    const ImageLayout &layout, std::uint16_t destination_y) {
    return map_destination_coordinate(
        destination_y,
        kDisplayHeight,
        layout.source_crop.y,
        layout.source_crop.height);
}

ClipResult map_cover_block(
    const ImageLayout &layout,
    const InclusiveRect &block,
    Region &destination) {
    destination = {};
    if (!valid_cover_layout(layout) ||
        block.left > block.right || block.top > block.bottom ||
        block.right >= layout.scaled_width ||
        block.bottom >= layout.scaled_height) {
        return ClipResult::invalid;
    }

    std::uint16_t first_x = kDisplayWidth;
    std::uint16_t last_x = 0;
    for (std::uint16_t x = 0; x < kDisplayWidth; ++x) {
        const std::uint16_t source = source_x_for_destination(layout, x);
        if (source >= block.left && source <= block.right) {
            first_x = std::min(first_x, x);
            last_x = x;
        }
    }
    std::uint16_t first_y = kDisplayHeight;
    std::uint16_t last_y = 0;
    for (std::uint16_t y = 0; y < kDisplayHeight; ++y) {
        const std::uint16_t source = source_y_for_destination(layout, y);
        if (source >= block.top && source <= block.bottom) {
            first_y = std::min(first_y, y);
            last_y = y;
        }
    }
    if (first_x == kDisplayWidth || first_y == kDisplayHeight) {
        return ClipResult::outside;
    }
    destination = {
        first_x,
        first_y,
        static_cast<std::uint16_t>(last_x - first_x + 1U),
        static_cast<std::uint16_t>(last_y - first_y + 1U),
    };
    return ClipResult::visible;
}

}  // namespace image
}  // namespace chatesp
