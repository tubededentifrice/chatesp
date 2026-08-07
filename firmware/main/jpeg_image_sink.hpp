#pragma once

#include <cstddef>
#include <cstdint>

#include "chatesp/agent_interfaces.hpp"
#include "chatesp/image_layout.hpp"
#include "image_frame.hpp"

namespace chatesp::image {

class JpegImageSink final : public agent::ByteSink {
public:
    explicit JpegImageSink(
        agent::CancellationToken &cancellation,
        std::size_t maximum_compressed_bytes =
            agent::Limits::max_image_download_bytes,
        std::uint32_t maximum_source_dimension = kMaximumSourceDimension);
    ~JpegImageSink() override;

    JpegImageSink(const JpegImageSink &) = delete;
    JpegImageSink &operator=(const JpegImageSink &) = delete;

    agent::Error begin(const agent::ImageMetadata &metadata) override;
    agent::Error write(
        const std::uint8_t *data, std::size_t size) override;
    agent::Error finish() override;
    void abort() override;

    [[nodiscard]] const Rgb565Frame &frame() const { return frame_; }
    [[nodiscard]] Rgb565Frame take_frame();
    [[nodiscard]] const ImageLayout &layout() const { return layout_; }
    [[nodiscard]] std::uint32_t source_width() const { return source_width_; }
    [[nodiscard]] std::uint32_t source_height() const { return source_height_; }
    [[nodiscard]] bool ready() const { return ready_; }

private:
    void release_compressed();
    agent::Error decode();

    agent::CancellationToken &cancellation_;
    std::size_t maximum_compressed_bytes_ = 0;
    std::uint32_t maximum_source_dimension_ = 0;
    std::uint8_t *compressed_ = nullptr;
    std::size_t expected_bytes_ = 0;
    std::size_t received_bytes_ = 0;
    std::uint32_t declared_width_ = 0;
    std::uint32_t declared_height_ = 0;
    std::uint32_t source_width_ = 0;
    std::uint32_t source_height_ = 0;
    Rgb565Frame frame_;
    ImageLayout layout_{};
    agent::Error write_error_ = agent::Error::none;
    bool started_ = false;
    bool ready_ = false;
};

}  // namespace chatesp::image
