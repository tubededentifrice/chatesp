#include <unity.h>

#include "chatesp/audio_buffer.hpp"
#include "chatesp/audio_level.hpp"
#include "chatesp/audio_session.hpp"

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

void test_pcm_peak_percent_uses_only_the_requested_tail() {
    const std::int16_t samples[] = {12'000, 0, -6'000, 3'000};

    TEST_ASSERT_EQUAL_UINT8(
        50, chatesp::pcm_peak_percent(samples, 4, 2));
}

void test_pcm_peak_percent_handles_full_negative_scale_and_invalid_input() {
    const std::int16_t samples[] = {-32'768};

    TEST_ASSERT_EQUAL_UINT8(
        100, chatesp::pcm_peak_percent(samples, 1, 1));
    TEST_ASSERT_EQUAL_UINT8(
        0, chatesp::pcm_peak_percent(nullptr, 1, 1));
    TEST_ASSERT_EQUAL_UINT8(
        0, chatesp::pcm_peak_percent(samples, 1, 1, 0));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_audio_sample_budget_has_a_strict_limit);
    RUN_TEST(test_audio_sample_budget_reset_removes_recorded_length);
    RUN_TEST(test_audio_session_gate_keeps_capture_and_playback_exclusive);
    RUN_TEST(test_wrong_owner_cannot_release_audio_session);
    RUN_TEST(test_pcm_peak_percent_uses_only_the_requested_tail);
    RUN_TEST(test_pcm_peak_percent_handles_full_negative_scale_and_invalid_input);
    return UNITY_END();
}
