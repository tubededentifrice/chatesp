#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace chatesp {
namespace runtime {

class Pcm16Stream {
public:
    static constexpr std::size_t kMaxChunkSamples = 480;

    enum class Status : std::uint8_t {
        none,
        invalid_argument,
        incomplete_sample,
        output_failed,
    };

    using Output = bool (*)(
        void *context, const std::int16_t *samples,
        std::size_t sample_count);

    Status write(
        const std::uint8_t *data, std::size_t size, Output output,
        void *context);
    [[nodiscard]] Status finish() const;
    void reset();

private:
    std::array<std::int16_t, kMaxChunkSamples> samples_{};
    std::uint8_t carry_ = 0;
    bool has_carry_ = false;
    bool output_failed_ = false;
};

}  // namespace runtime
}  // namespace chatesp
