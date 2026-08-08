#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace chatesp {

constexpr std::size_t kAudioSpectrumBandCount = 18;
constexpr std::uint32_t kAudioSpectrumSampleRateHz = 16'000;
constexpr std::size_t kAudioSpectrumWindowSamples = 256;
using AudioSpectrum =
    std::array<std::uint8_t, kAudioSpectrumBandCount>;

// The boundaries assign each positive-frequency FFT bin from 125 Hz through
// 8 kHz to exactly one band. Log-like spacing gives speech frequencies more
// detail while continuous coverage prevents gaps at high frequencies.
constexpr std::array<std::uint8_t, kAudioSpectrumBandCount + 1>
    kAudioSpectrumBinBoundaries{
        2, 3, 4, 5, 7, 9, 12, 16, 21, 28,
        37, 48, 60, 73, 86, 99, 112, 122, 129,
    };

inline void fft_in_place(
    std::array<float, kAudioSpectrumWindowSamples> &real,
    std::array<float, kAudioSpectrumWindowSamples> &imaginary) {
    for (std::size_t index = 1, reversed = 0;
         index < real.size(); ++index) {
        std::size_t bit = real.size() >> 1U;
        while ((reversed & bit) != 0U) {
            reversed ^= bit;
            bit >>= 1U;
        }
        reversed ^= bit;
        if (index < reversed) {
            std::swap(real[index], real[reversed]);
            std::swap(imaginary[index], imaginary[reversed]);
        }
    }

    constexpr float kPi = 3.14159265358979323846F;
    for (std::size_t length = 2; length <= real.size(); length <<= 1U) {
        const float angle = -2.0F * kPi / static_cast<float>(length);
        const float step_real = std::cos(angle);
        const float step_imaginary = std::sin(angle);
        for (std::size_t first = 0; first < real.size(); first += length) {
            float rotation_real = 1.0F;
            float rotation_imaginary = 0.0F;
            for (std::size_t offset = 0; offset < length / 2; ++offset) {
                const std::size_t even = first + offset;
                const std::size_t odd = even + length / 2;
                const float odd_real =
                    real[odd] * rotation_real -
                    imaginary[odd] * rotation_imaginary;
                const float odd_imaginary =
                    real[odd] * rotation_imaginary +
                    imaginary[odd] * rotation_real;
                real[odd] = real[even] - odd_real;
                imaginary[odd] = imaginary[even] - odd_imaginary;
                real[even] += odd_real;
                imaginary[even] += odd_imaginary;

                const float next_rotation_real =
                    rotation_real * step_real -
                    rotation_imaginary * step_imaginary;
                rotation_imaginary =
                    rotation_real * step_imaginary +
                    rotation_imaginary * step_real;
                rotation_real = next_rotation_real;
            }
        }
    }
}

inline AudioSpectrum pcm_frequency_spectrum(
    const std::int16_t *samples,
    std::size_t sample_count,
    std::size_t tail_count = kAudioSpectrumWindowSamples,
    float full_scale_peak = 12'000.0F) {
    AudioSpectrum levels{};
    if (samples == nullptr || sample_count == 0 || tail_count == 0 ||
        !(full_scale_peak > 0.0F)) {
        return levels;
    }

    tail_count = std::min(
        {sample_count, tail_count, kAudioSpectrumWindowSamples});
    const std::size_t first = sample_count - tail_count;

    std::int64_t sum = 0;
    for (std::size_t index = first; index < sample_count; ++index) {
        sum += samples[index];
    }
    const float mean =
        static_cast<float>(sum) / static_cast<float>(tail_count);
    std::array<float, kAudioSpectrumWindowSamples> real{};
    std::array<float, kAudioSpectrumWindowSamples> imaginary{};
    const std::size_t padding = real.size() - tail_count;
    const float center = static_cast<float>(tail_count - 1) * 0.5F;
    const float window_radius = center + 1.0F;
    float window_sum = 0.0F;
    for (std::size_t offset = 0; offset < tail_count; ++offset) {
        const float distance =
            std::fabs(static_cast<float>(offset) - center);
        const float window = 1.0F - distance / window_radius;
        real[padding + offset] =
            (static_cast<float>(samples[first + offset]) - mean) * window;
        window_sum += window;
    }
    fft_in_place(real, imaginary);

    for (std::size_t band = 0; band < levels.size(); ++band) {
        float band_power = 0.0F;
        for (std::size_t bin = kAudioSpectrumBinBoundaries[band];
             bin < kAudioSpectrumBinBoundaries[band + 1]; ++bin) {
            band_power +=
                real[bin] * real[bin] + imaginary[bin] * imaginary[bin];
        }
        const float amplitude = window_sum > 0.0F
            ? 2.0F * std::sqrt(band_power) / window_sum
            : 0.0F;
        if (!(amplitude > 0.0F)) {
            continue;
        }

        // The logarithmic display floor removes normal room noise. The upper
        // limit leaves some headroom before microphone clipping.
        constexpr float kMinimumDb = -48.0F;
        constexpr float kMaximumDb = -4.0F;
        const float decibels =
            20.0F * std::log10(amplitude / full_scale_peak);
        const float scaled =
            (decibels - kMinimumDb) * 100.0F /
            (kMaximumDb - kMinimumDb);
        levels[band] = static_cast<std::uint8_t>(
            std::max(0.0F, std::min(100.0F, scaled)) + 0.5F);
    }
    return levels;
}

}  // namespace chatesp
