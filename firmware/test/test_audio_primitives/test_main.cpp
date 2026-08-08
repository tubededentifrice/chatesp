#include <algorithm>
#include <array>
#include <cmath>

#include <unity.h>

#include "chatesp/audio_buffer.hpp"
#include "chatesp/audio_session.hpp"
#include "chatesp/audio_spectrum.hpp"

void setUp() {}
void tearDown() {}

void test_audio_sample_budget_has_a_strict_limit() {
    chatesp::AudioSampleBudget budget{640};

    TEST_ASSERT_TRUE(budget.commit(320));
    TEST_ASSERT_EQUAL_UINT32(320, budget.used());
    TEST_ASSERT_TRUE(budget.commit(320));
    TEST_ASSERT_EQUAL_UINT32(0, budget.remaining());
    TEST_ASSERT_FALSE(budget.commit(1));
    TEST_ASSERT_EQUAL_UINT32(640, budget.used());
}

void test_audio_sample_budget_reset_removes_recorded_length() {
    chatesp::AudioSampleBudget budget{320};

    TEST_ASSERT_TRUE(budget.commit(200));
    budget.reset();
    TEST_ASSERT_EQUAL_UINT32(0, budget.used());
    TEST_ASSERT_EQUAL_UINT32(320, budget.remaining());
}

void test_audio_session_gate_keeps_capture_and_playback_exclusive() {
    chatesp::AudioSessionGate gate;

    TEST_ASSERT_TRUE(gate.try_acquire(chatesp::AudioSession::capture));
    TEST_ASSERT_FALSE(gate.try_acquire(chatesp::AudioSession::playback));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::AudioSession::capture),
        static_cast<int>(gate.current()));

    gate.release(chatesp::AudioSession::capture);
    TEST_ASSERT_TRUE(gate.try_acquire(chatesp::AudioSession::playback));
    gate.release(chatesp::AudioSession::playback);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::AudioSession::none),
        static_cast<int>(gate.current()));
}

void test_wrong_owner_cannot_release_audio_session() {
    chatesp::AudioSessionGate gate;

    TEST_ASSERT_TRUE(gate.try_acquire(chatesp::AudioSession::capture));
    gate.release(chatesp::AudioSession::playback);
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(chatesp::AudioSession::capture),
        static_cast<int>(gate.current()));
}

std::array<std::int16_t, chatesp::kAudioSpectrumWindowSamples> tone(
    float frequency_hz, float amplitude = 6'000.0F) {
    std::array<std::int16_t, chatesp::kAudioSpectrumWindowSamples> samples{};
    constexpr float kSampleRateHz = 16'000.0F;
    constexpr float kPi = 3.14159265358979323846F;
    for (std::size_t index = 0; index < samples.size(); ++index) {
        samples[index] = static_cast<std::int16_t>(
            amplitude * std::sin(
                2.0F * kPi * frequency_hz *
                static_cast<float>(index) / kSampleRateHz));
    }
    return samples;
}

void test_pcm_frequency_spectrum_rejects_invalid_input_and_silence() {
    const auto invalid = chatesp::pcm_frequency_spectrum(nullptr, 10);
    for (const std::uint8_t level : invalid) {
        TEST_ASSERT_EQUAL_UINT8(0, level);
    }

    std::array<std::int16_t, chatesp::kAudioSpectrumWindowSamples> silence{};
    const auto silent = chatesp::pcm_frequency_spectrum(
        silence.data(), silence.size());
    for (const std::uint8_t level : silent) {
        TEST_ASSERT_EQUAL_UINT8(0, level);
    }
}

void test_pcm_frequency_spectrum_places_speech_tone_in_expected_band() {
    const auto samples = tone(1'030.0F);
    const auto levels = chatesp::pcm_frequency_spectrum(
        samples.data(), samples.size());

    TEST_ASSERT_GREATER_THAN_UINT8(85, levels[7]);
    TEST_ASSERT_GREATER_THAN_UINT8(levels[6] + 20, levels[7]);
    TEST_ASSERT_GREATER_THAN_UINT8(levels[8] + 20, levels[7]);
}

void test_pcm_frequency_spectrum_places_high_tone_in_expected_band() {
    const auto samples = tone(6'400.0F);
    const auto levels = chatesp::pcm_frequency_spectrum(
        samples.data(), samples.size());

    TEST_ASSERT_GREATER_THAN_UINT8(85, levels[15]);
    TEST_ASSERT_GREATER_THAN_UINT8(levels[14] + 20, levels[15]);
    TEST_ASSERT_GREATER_THAN_UINT8(levels[16] + 20, levels[15]);
}

void test_pcm_frequency_spectrum_has_no_gap_between_high_bands() {
    const auto samples = tone(6'000.0F);
    const auto levels = chatesp::pcm_frequency_spectrum(
        samples.data(), samples.size());

    TEST_ASSERT_GREATER_THAN_UINT8(85, levels[14]);
    TEST_ASSERT_GREATER_THAN_UINT8(levels[13] + 20, levels[14]);
    TEST_ASSERT_GREATER_THAN_UINT8(levels[15] + 20, levels[14]);
}

void test_pcm_frequency_spectrum_uses_only_the_requested_tail() {
    auto samples = tone(1'030.0F);
    std::fill(samples.begin() + samples.size() / 2, samples.end(), 0);
    const auto levels = chatesp::pcm_frequency_spectrum(
        samples.data(), samples.size(), samples.size() / 2);

    for (const std::uint8_t level : levels) {
        TEST_ASSERT_EQUAL_UINT8(0, level);
    }
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_audio_sample_budget_has_a_strict_limit);
    RUN_TEST(test_audio_sample_budget_reset_removes_recorded_length);
    RUN_TEST(test_audio_session_gate_keeps_capture_and_playback_exclusive);
    RUN_TEST(test_wrong_owner_cannot_release_audio_session);
    RUN_TEST(test_pcm_frequency_spectrum_rejects_invalid_input_and_silence);
    RUN_TEST(test_pcm_frequency_spectrum_places_speech_tone_in_expected_band);
    RUN_TEST(test_pcm_frequency_spectrum_places_high_tone_in_expected_band);
    RUN_TEST(test_pcm_frequency_spectrum_has_no_gap_between_high_bands);
    RUN_TEST(test_pcm_frequency_spectrum_uses_only_the_requested_tail);
    return UNITY_END();
}
