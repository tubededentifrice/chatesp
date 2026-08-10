#include <cstdint>
#include <limits>

#include <unity.h>

#include "chatesp/image_layout.hpp"

using chatesp::image::ClipResult;
using chatesp::image::ImageLayout;
using chatesp::image::InclusiveRect;

namespace {

void assert_clip(ClipResult expected, ClipResult actual) {
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(expected), static_cast<int>(actual));
}

void assert_region(
    const chatesp::image::Region &region,
    std::uint16_t x,
    std::uint16_t y,
    std::uint16_t width,
    std::uint16_t height) {
    TEST_ASSERT_EQUAL_UINT16(x, region.x);
    TEST_ASSERT_EQUAL_UINT16(y, region.y);
    TEST_ASSERT_EQUAL_UINT16(width, region.width);
    TEST_ASSERT_EQUAL_UINT16(height, region.height);
}

void test_layout_rejects_invalid_and_unbounded_dimensions() {
    ImageLayout layout;
    TEST_ASSERT_FALSE(chatesp::image::plan_jpeg_layout(
        0, 448, chatesp::image::kMaximumSourceDimension, layout));
    TEST_ASSERT_FALSE(chatesp::image::plan_jpeg_layout(
        368, 0, chatesp::image::kMaximumSourceDimension, layout));
    TEST_ASSERT_FALSE(chatesp::image::plan_jpeg_layout(368, 448, 0, layout));
    TEST_ASSERT_FALSE(chatesp::image::plan_jpeg_layout(
        368, 448, chatesp::image::kMaximumSourceDimension + 1, layout));
    TEST_ASSERT_FALSE(chatesp::image::plan_jpeg_layout(
        chatesp::image::kMaximumSourceDimension + 1,
        448,
        chatesp::image::kMaximumSourceDimension,
        layout));
    TEST_ASSERT_FALSE(chatesp::image::plan_jpeg_layout(
        1'001, 900, 1'000, layout));
    TEST_ASSERT_FALSE(chatesp::image::plan_jpeg_layout(
        std::numeric_limits<std::uint32_t>::max(),
        std::numeric_limits<std::uint32_t>::max(),
        chatesp::image::kMaximumSourceDimension,
        layout));
    TEST_ASSERT_EQUAL_UINT16(0, layout.scaled_width);
    TEST_ASSERT_EQUAL_UINT16(0, layout.scaled_height);
}

void test_exact_display_has_no_scale_crop_or_offset() {
    ImageLayout layout;
    TEST_ASSERT_TRUE(chatesp::image::plan_jpeg_layout(
        368, 448, chatesp::image::kMaximumSourceDimension, layout));
    TEST_ASSERT_EQUAL_UINT8(0, layout.jpeg_scale);
    TEST_ASSERT_EQUAL_UINT16(368, layout.scaled_width);
    TEST_ASSERT_EQUAL_UINT16(448, layout.scaled_height);
    assert_region(layout.source_crop, 0, 0, 368, 448);
    assert_region(layout.destination, 0, 0, 368, 448);
}

void test_layout_selects_largest_scale_that_still_covers_display() {
    ImageLayout layout;
    TEST_ASSERT_TRUE(chatesp::image::plan_jpeg_layout(
        736, 896, chatesp::image::kMaximumSourceDimension, layout));
    TEST_ASSERT_EQUAL_UINT8(1, layout.jpeg_scale);
    TEST_ASSERT_EQUAL_UINT16(368, layout.scaled_width);
    TEST_ASSERT_EQUAL_UINT16(448, layout.scaled_height);

    TEST_ASSERT_TRUE(chatesp::image::plan_jpeg_layout(
        1'472, 1'792, chatesp::image::kMaximumSourceDimension, layout));
    TEST_ASSERT_EQUAL_UINT8(2, layout.jpeg_scale);
    TEST_ASSERT_EQUAL_UINT16(368, layout.scaled_width);
    TEST_ASSERT_EQUAL_UINT16(448, layout.scaled_height);

    TEST_ASSERT_TRUE(chatesp::image::plan_jpeg_layout(
        735, 895, chatesp::image::kMaximumSourceDimension, layout));
    TEST_ASSERT_EQUAL_UINT8(0, layout.jpeg_scale);

    TEST_ASSERT_TRUE(chatesp::image::plan_jpeg_layout(
        737, 897, chatesp::image::kMaximumSourceDimension, layout));
    TEST_ASSERT_EQUAL_UINT8(1, layout.jpeg_scale);
    TEST_ASSERT_EQUAL_UINT16(368, layout.scaled_width);
    TEST_ASSERT_EQUAL_UINT16(448, layout.scaled_height);
}

void test_large_square_is_scaled_and_center_cropped() {
    ImageLayout layout;
    TEST_ASSERT_TRUE(chatesp::image::plan_jpeg_layout(
        2'048, 2'048, chatesp::image::kMaximumSourceDimension, layout));
    TEST_ASSERT_EQUAL_UINT8(2, layout.jpeg_scale);
    TEST_ASSERT_EQUAL_UINT16(512, layout.scaled_width);
    TEST_ASSERT_EQUAL_UINT16(512, layout.scaled_height);
    assert_region(layout.source_crop, 46, 0, 420, 512);
    assert_region(layout.destination, 0, 0, 368, 448);
}

void test_small_and_one_axis_images_always_cover_the_display() {
    ImageLayout layout;
    TEST_ASSERT_TRUE(chatesp::image::plan_jpeg_layout(
        100, 80, chatesp::image::kMaximumSourceDimension, layout));
    TEST_ASSERT_EQUAL_UINT8(0, layout.jpeg_scale);
    assert_region(layout.source_crop, 17, 0, 65, 80);
    assert_region(layout.destination, 0, 0, 368, 448);

    TEST_ASSERT_TRUE(chatesp::image::plan_jpeg_layout(
        800, 300, chatesp::image::kMaximumSourceDimension, layout));
    assert_region(layout.source_crop, 277, 0, 246, 300);
    assert_region(layout.destination, 0, 0, 368, 448);

    TEST_ASSERT_TRUE(chatesp::image::plan_jpeg_layout(
        300, 800, chatesp::image::kMaximumSourceDimension, layout));
    assert_region(layout.source_crop, 0, 217, 300, 365);
    assert_region(layout.destination, 0, 0, 368, 448);
}

void test_rgb888_conversion_has_native_rgb565_values() {
    TEST_ASSERT_EQUAL_HEX16(
        0x0000, chatesp::image::rgb888_to_rgb565(0, 0, 0));
    TEST_ASSERT_EQUAL_HEX16(
        0xF800, chatesp::image::rgb888_to_rgb565(255, 0, 0));
    TEST_ASSERT_EQUAL_HEX16(
        0x07E0, chatesp::image::rgb888_to_rgb565(0, 255, 0));
    TEST_ASSERT_EQUAL_HEX16(
        0x001F, chatesp::image::rgb888_to_rgb565(0, 0, 255));
    TEST_ASSERT_EQUAL_HEX16(
        0xFFFF, chatesp::image::rgb888_to_rgb565(255, 255, 255));
    TEST_ASSERT_EQUAL_HEX16(
        0x8410, chatesp::image::rgb888_to_rgb565(128, 128, 128));
}

void test_rom_rgb888_conversion_keeps_red_and_blue() {
    TEST_ASSERT_EQUAL_HEX16(
        0xF800, chatesp::image::rom_rgb888_to_rgb565(255, 0, 0));
    TEST_ASSERT_EQUAL_HEX16(
        0x001F, chatesp::image::rom_rgb888_to_rgb565(0, 0, 255));
}

void test_cover_map_reports_a_visible_destination_region() {
    ImageLayout layout;
    TEST_ASSERT_TRUE(chatesp::image::plan_jpeg_layout(
        2'048, 2'048, chatesp::image::kMaximumSourceDimension, layout));
    chatesp::image::Region destination;
    assert_clip(
        ClipResult::visible,
        chatesp::image::map_cover_block(
            layout, {46, 0, 61, 15}, destination));
    TEST_ASSERT_EQUAL_UINT16(0, destination.x);
    TEST_ASSERT_EQUAL_UINT16(0, destination.y);
    TEST_ASSERT_GREATER_THAN_UINT16(0, destination.width);
    TEST_ASSERT_GREATER_THAN_UINT16(0, destination.height);
}

void test_cover_map_distinguishes_outside_and_invalid_blocks() {
    ImageLayout layout;
    TEST_ASSERT_TRUE(chatesp::image::plan_jpeg_layout(
        2'048, 2'048, chatesp::image::kMaximumSourceDimension, layout));
    chatesp::image::Region destination;
    assert_clip(
        ClipResult::outside,
        chatesp::image::map_cover_block(
            layout, {0, 0, 15, 15}, destination));
    TEST_ASSERT_EQUAL_UINT16(0, destination.width);
    TEST_ASSERT_EQUAL_UINT16(0, destination.height);

    assert_clip(
        ClipResult::invalid,
        chatesp::image::map_cover_block(
            layout, {10, 10, 9, 20}, destination));
    assert_clip(
        ClipResult::invalid,
        chatesp::image::map_cover_block(
            layout, {496, 496, 512, 511}, destination));

    ImageLayout invalid_layout = layout;
    invalid_layout.destination.width = 367;
    assert_clip(
        ClipResult::invalid,
        chatesp::image::map_cover_block(
            invalid_layout, {46, 0, 61, 15}, destination));
}

void test_cover_map_maps_the_last_display_pixel_without_overflow() {
    ImageLayout layout;
    TEST_ASSERT_TRUE(chatesp::image::plan_jpeg_layout(
        2'048, 2'048, chatesp::image::kMaximumSourceDimension, layout));
    const std::uint16_t source_x =
        chatesp::image::source_x_for_destination(layout, 367);
    const std::uint16_t source_y =
        chatesp::image::source_y_for_destination(layout, 447);
    chatesp::image::Region destination;
    assert_clip(
        ClipResult::visible,
        chatesp::image::map_cover_block(
            layout,
            {source_x, source_y, source_x, source_y},
            destination));
    TEST_ASSERT_EQUAL_UINT16(
        367, destination.x + destination.width - 1U);
    TEST_ASSERT_EQUAL_UINT16(
        447, destination.y + destination.height - 1U);
}

void test_cover_map_upscales_a_small_image_to_the_display_edges() {
    ImageLayout layout;
    TEST_ASSERT_TRUE(chatesp::image::plan_jpeg_layout(
        100, 80, chatesp::image::kMaximumSourceDimension, layout));
    chatesp::image::Region destination;
    assert_clip(
        ClipResult::visible,
        chatesp::image::map_cover_block(
            layout, {17, 0, 32, 15}, destination));
    TEST_ASSERT_EQUAL_UINT16(0, destination.x);
    TEST_ASSERT_EQUAL_UINT16(0, destination.y);
    TEST_ASSERT_GREATER_THAN_UINT16(0, destination.width);
    TEST_ASSERT_GREATER_THAN_UINT16(0, destination.height);
}

}  // namespace

void setUp() {}
void tearDown() {}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_layout_rejects_invalid_and_unbounded_dimensions);
    RUN_TEST(test_exact_display_has_no_scale_crop_or_offset);
    RUN_TEST(test_layout_selects_largest_scale_that_still_covers_display);
    RUN_TEST(test_large_square_is_scaled_and_center_cropped);
    RUN_TEST(test_small_and_one_axis_images_always_cover_the_display);
    RUN_TEST(test_rgb888_conversion_has_native_rgb565_values);
    RUN_TEST(test_rom_rgb888_conversion_keeps_red_and_blue);
    RUN_TEST(test_cover_map_reports_a_visible_destination_region);
    RUN_TEST(test_cover_map_distinguishes_outside_and_invalid_blocks);
    RUN_TEST(test_cover_map_maps_the_last_display_pixel_without_overflow);
    RUN_TEST(test_cover_map_upscales_a_small_image_to_the_display_edges);
    return UNITY_END();
}
