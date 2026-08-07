#include "jpeg_image_sink.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include "esp32s3/rom/tjpgd.h"
#include "esp_heap_caps.h"

namespace chatesp::image {
namespace {

constexpr std::size_t kDecoderWorkBytes = 16 * 1024;

struct DecodeContext {
    const std::uint8_t *compressed = nullptr;
    std::size_t compressed_size = 0;
    std::size_t read_offset = 0;
    Rgb565Frame *frame = nullptr;
    const ImageLayout *layout = nullptr;
    agent::CancellationToken *cancellation = nullptr;
    bool cancelled = false;
    bool output_invalid = false;
};

UINT read_jpeg(JDEC *decoder, BYTE *output, UINT requested) {
    auto *context = static_cast<DecodeContext *>(decoder->device);
    if (context == nullptr || context->compressed == nullptr ||
        context->read_offset > context->compressed_size) {
        return 0;
    }
    if (context->cancellation != nullptr &&
        context->cancellation->cancelled()) {
        context->cancelled = true;
        return 0;
    }
    const std::size_t count = std::min<std::size_t>(
        requested, context->compressed_size - context->read_offset);
    if (count != 0 && output != nullptr) {
        std::memcpy(
            output, context->compressed + context->read_offset, count);
    }
    context->read_offset += count;
    return static_cast<UINT>(count);
}

UINT write_jpeg(JDEC *decoder, void *bitmap, JRECT *rectangle) {
    auto *context = static_cast<DecodeContext *>(decoder->device);
    if (context == nullptr || context->frame == nullptr ||
        context->layout == nullptr || bitmap == nullptr ||
        rectangle == nullptr || !context->frame->available()) {
        if (context != nullptr) {
            context->output_invalid = true;
        }
        return 0;
    }
    if (context->cancellation != nullptr &&
        context->cancellation->cancelled()) {
        context->cancelled = true;
        return 0;
    }

    const InclusiveRect block{
        rectangle->left,
        rectangle->top,
        rectangle->right,
        rectangle->bottom,
    };
    Region destination_region;
    const ClipResult clip = map_cover_block(
        *context->layout, block, destination_region);
    if (clip == ClipResult::outside) {
        return 1;
    }
    if (clip != ClipResult::visible) {
        context->output_invalid = true;
        return 0;
    }

    const std::size_t block_width =
        static_cast<std::size_t>(block.right - block.left) + 1U;
    const auto *source = static_cast<const std::uint8_t *>(bitmap);
    std::uint16_t *destination = context->frame->data();
    for (std::size_t row = 0; row < destination_region.height; ++row) {
        if (context->cancellation != nullptr &&
            context->cancellation->cancelled()) {
            context->cancelled = true;
            return 0;
        }
        const std::uint16_t destination_y =
            static_cast<std::uint16_t>(destination_region.y + row);
        const std::uint16_t source_y = source_y_for_destination(
            *context->layout, destination_y);
        std::size_t destination_index =
            static_cast<std::size_t>(destination_y) *
                kDisplayWidth +
            destination_region.x;
        for (std::size_t column = 0;
             column < destination_region.width;
             ++column) {
            const std::uint16_t destination_x = static_cast<std::uint16_t>(
                destination_region.x + column);
            const std::uint16_t source_x = source_x_for_destination(
                *context->layout, destination_x);
            const std::size_t source_index =
                ((static_cast<std::size_t>(source_y) - block.top) *
                     block_width +
                 (static_cast<std::size_t>(source_x) - block.left)) *
                3U;
            // The ESP32-S3 ROM writes R, G, B bytes for RGB888 output.
            destination[destination_index + column] = rom_rgb888_to_rgb565(
                source[source_index],
                source[source_index + 1U],
                source[source_index + 2U]);
        }
    }
    return 1;
}

agent::Error map_decoder_error(
    JRESULT result, const DecodeContext &context) {
    if (context.cancelled) {
        return agent::Error::cancelled;
    }
    if (context.output_invalid) {
        return agent::Error::malformed_response;
    }
    switch (result) {
        case JDR_OK:
            return agent::Error::none;
        case JDR_MEM1:
        case JDR_MEM2:
            return agent::Error::model_failed;
        case JDR_FMT2:
        case JDR_FMT3:
            return agent::Error::unsupported_media;
        case JDR_INTR:
        case JDR_INP:
        case JDR_PAR:
        case JDR_FMT1:
            return agent::Error::malformed_response;
    }
    return agent::Error::malformed_response;
}

}  // namespace

JpegImageSink::JpegImageSink(
    agent::CancellationToken &cancellation,
    std::size_t maximum_compressed_bytes,
    std::uint32_t maximum_source_dimension)
    : cancellation_(cancellation),
      maximum_compressed_bytes_(maximum_compressed_bytes),
      maximum_source_dimension_(maximum_source_dimension) {}

JpegImageSink::~JpegImageSink() { abort(); }

Rgb565Frame JpegImageSink::take_frame() {
    if (!ready_) {
        return {};
    }
    ready_ = false;
    return std::move(frame_);
}

agent::Error JpegImageSink::begin(const agent::ImageMetadata &metadata) {
    abort();
    if (metadata.media_type != agent::ImageMediaType::jpeg) {
        return agent::Error::unsupported_media;
    }
    if (maximum_compressed_bytes_ == 0 ||
        maximum_compressed_bytes_ > agent::Limits::max_image_download_bytes ||
        metadata.content_length == 0 ||
        metadata.content_length > maximum_compressed_bytes_ ||
        maximum_source_dimension_ == 0 ||
        maximum_source_dimension_ > kMaximumSourceDimension) {
        return agent::Error::limit_exceeded;
    }
    if ((metadata.width != 0 &&
         metadata.width > maximum_source_dimension_) ||
        (metadata.height != 0 &&
         metadata.height > maximum_source_dimension_)) {
        return agent::Error::limit_exceeded;
    }
    if (cancellation_.cancelled()) {
        return agent::Error::cancelled;
    }
    if (!frame_.allocate()) {
        return agent::Error::model_failed;
    }
    compressed_ = static_cast<std::uint8_t *>(heap_caps_malloc(
        metadata.content_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (compressed_ == nullptr) {
        frame_.reset();
        return agent::Error::model_failed;
    }

    expected_bytes_ = metadata.content_length;
    declared_width_ = metadata.width;
    declared_height_ = metadata.height;
    started_ = true;
    return agent::Error::none;
}

agent::Error JpegImageSink::write(
    const std::uint8_t *data, std::size_t size) {
    if (!started_ || (size != 0 && data == nullptr)) {
        return agent::Error::invalid_argument;
    }
    if (write_error_ != agent::Error::none) {
        return write_error_;
    }
    if (cancellation_.cancelled()) {
        write_error_ = agent::Error::cancelled;
        return write_error_;
    }
    if (size > expected_bytes_ - received_bytes_) {
        write_error_ = agent::Error::limit_exceeded;
        return write_error_;
    }
    if (size != 0) {
        std::memcpy(compressed_ + received_bytes_, data, size);
        received_bytes_ += size;
    }
    return agent::Error::none;
}

agent::Error JpegImageSink::finish() {
    if (!started_) {
        return agent::Error::invalid_argument;
    }
    if (write_error_ != agent::Error::none) {
        return write_error_;
    }
    if (received_bytes_ != expected_bytes_) {
        return agent::Error::malformed_response;
    }
    if (cancellation_.cancelled()) {
        return agent::Error::cancelled;
    }
    const agent::Error result = decode();
    if (result == agent::Error::none) {
        ready_ = true;
        started_ = false;
        release_compressed();
    }
    return result;
}

void JpegImageSink::abort() {
    release_compressed();
    frame_.reset();
    layout_ = {};
    expected_bytes_ = 0;
    received_bytes_ = 0;
    declared_width_ = 0;
    declared_height_ = 0;
    source_width_ = 0;
    source_height_ = 0;
    write_error_ = agent::Error::none;
    started_ = false;
    ready_ = false;
}

void JpegImageSink::release_compressed() {
    if (compressed_ != nullptr) {
        std::memset(compressed_, 0, expected_bytes_);
        heap_caps_free(compressed_);
        compressed_ = nullptr;
    }
}

agent::Error JpegImageSink::decode() {
    void *work = heap_caps_malloc(
        kDecoderWorkBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (work == nullptr) {
        return agent::Error::model_failed;
    }

    DecodeContext context;
    context.compressed = compressed_;
    context.compressed_size = received_bytes_;
    context.frame = &frame_;
    context.cancellation = &cancellation_;
    JDEC decoder{};
    JRESULT decoder_result = jd_prepare(
        &decoder,
        read_jpeg,
        work,
        static_cast<UINT>(kDecoderWorkBytes),
        &context);
    agent::Error result = map_decoder_error(decoder_result, context);
    if (result == agent::Error::none) {
        source_width_ = decoder.width;
        source_height_ = decoder.height;
        if ((declared_width_ != 0 && declared_width_ != source_width_) ||
            (declared_height_ != 0 && declared_height_ != source_height_)) {
            result = agent::Error::malformed_response;
        } else if (!plan_jpeg_layout(
                       source_width_,
                       source_height_,
                       maximum_source_dimension_,
                       layout_)) {
            result = agent::Error::limit_exceeded;
        }
    }
    if (result == agent::Error::none && cancellation_.cancelled()) {
        context.cancelled = true;
        result = agent::Error::cancelled;
    }
    if (result == agent::Error::none) {
        context.layout = &layout_;
        frame_.clear();
        decoder_result = jd_decomp(&decoder, write_jpeg, layout_.jpeg_scale);
        result = map_decoder_error(decoder_result, context);
        if (result == agent::Error::none && cancellation_.cancelled()) {
            result = agent::Error::cancelled;
        }
    }

    std::memset(work, 0, kDecoderWorkBytes);
    heap_caps_free(work);
    return result;
}

}  // namespace chatesp::image
