#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>

#include <unity.h>

#include "../../main/device_memory_store.hpp"
#include "../../main/device_preferences_store.hpp"
#include "fake_platform.hpp"

namespace {

using namespace std::chrono_literals;

struct BlockingCallback {
    chatesp::DeviceMemoryStore *store = nullptr;
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    std::atomic<std::size_t> calls{0};
    std::atomic<bool> snapshot_succeeded{false};
};

void blocking_callback(
    const chatesp::agent::MemorySnapshot &, void *context) {
    auto &state = *static_cast<BlockingCallback *>(context);
    chatesp::agent::MemorySnapshot snapshot;
    state.snapshot_succeeded.store(
        state.store->snapshot(snapshot) == chatesp::agent::Error::none);
    state.calls.fetch_add(1);
    std::unique_lock<std::mutex> lock(state.mutex);
    state.entered = true;
    state.condition.notify_all();
    state.condition.wait(lock, [&state] { return state.release; });
}

void test_memory_initialization_retries_after_nvs_failure() {
    fake_platform::reset();
    fake_platform::set_nvs_init_failure_count(1, ESP_FAIL);

    {
        chatesp::DeviceMemoryStore store;
        TEST_ASSERT_EQUAL(ESP_FAIL, store.initialize());
        TEST_ASSERT_EQUAL(ESP_OK, store.initialize());
        TEST_ASSERT_EQUAL(ESP_OK, store.initialize());
        TEST_ASSERT_EQUAL_size_t(2, fake_platform::nvs_init_call_count());
        TEST_ASSERT_EQUAL_size_t(2, fake_platform::mutex_allocation_count());
    }
    TEST_ASSERT_EQUAL_size_t(
        fake_platform::mutex_allocation_count(),
        fake_platform::mutex_delete_count());
}

void test_memory_initialization_recovers_from_callback_mutex_failure() {
    fake_platform::reset();

    chatesp::DeviceMemoryStore store;
    fake_platform::fail_mutex_allocation_on_call(2);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, store.initialize());
    TEST_ASSERT_EQUAL_size_t(1, fake_platform::mutex_delete_count());
    TEST_ASSERT_EQUAL(ESP_OK, store.initialize());
}

void test_callback_clear_waits_and_blocks_later_callbacks() {
    fake_platform::reset();
    chatesp::DeviceMemoryStore store;
    TEST_ASSERT_EQUAL(ESP_OK, store.initialize());

    BlockingCallback callback_state;
    callback_state.store = &store;
    store.set_change_callback(blocking_callback, &callback_state);

    chatesp::agent::MemoryMutationResult first_result;
    std::atomic<int> mutation_error{
        static_cast<int>(chatesp::agent::Error::tool_failed)};
    std::thread mutation([&] {
        constexpr char fact[] = "First fact";
        mutation_error.store(static_cast<int>(store.remember(
            fact, sizeof(fact) - 1, first_result)));
    });

    bool callback_entered = false;
    {
        std::unique_lock<std::mutex> lock(callback_state.mutex);
        callback_entered = callback_state.condition.wait_for(
            lock, 2s, [&] { return callback_state.entered; });
        if (!callback_entered) {
            callback_state.release = true;
        }
    }
    if (!callback_entered) {
        callback_state.condition.notify_all();
        mutation.join();
    }
    TEST_ASSERT_TRUE(callback_entered);
    TEST_ASSERT_TRUE(callback_state.snapshot_succeeded.load());

    std::mutex clear_state_mutex;
    std::condition_variable clear_state_condition;
    bool clear_started = false;
    std::atomic<bool> clear_returned{false};
    std::thread clear_callback([&] {
        {
            std::lock_guard<std::mutex> lock(clear_state_mutex);
            clear_started = true;
        }
        clear_state_condition.notify_all();
        store.set_change_callback(nullptr, nullptr);
        clear_returned.store(true);
    });
    {
        std::unique_lock<std::mutex> lock(clear_state_mutex);
        (void)clear_state_condition.wait_for(
            lock, 2s, [&] { return clear_started; });
    }
    std::this_thread::sleep_for(20ms);
    TEST_ASSERT_FALSE(clear_returned.load());

    {
        std::lock_guard<std::mutex> lock(callback_state.mutex);
        callback_state.release = true;
    }
    callback_state.condition.notify_all();
    mutation.join();
    clear_callback.join();
    TEST_ASSERT_EQUAL(
        static_cast<int>(chatesp::agent::Error::none),
        mutation_error.load());
    TEST_ASSERT_TRUE(clear_returned.load());
    TEST_ASSERT_EQUAL_size_t(1, callback_state.calls.load());

    chatesp::agent::MemoryMutationResult second_result;
    constexpr char second_fact[] = "Second fact";
    TEST_ASSERT_EQUAL(
        static_cast<int>(chatesp::agent::Error::none),
        static_cast<int>(store.remember(
            second_fact, sizeof(second_fact) - 1, second_result)));
    TEST_ASSERT_EQUAL_size_t(1, callback_state.calls.load());
}

void test_preferences_initialization_retries_after_nvs_failure() {
    fake_platform::reset();
    fake_platform::set_nvs_init_failure_count(1, ESP_FAIL);

    chatesp::DevicePreferencesStore store;
    TEST_ASSERT_EQUAL(ESP_FAIL, store.initialize());
    TEST_ASSERT_FALSE(store.persistent());
    TEST_ASSERT_EQUAL(ESP_OK, store.initialize());
    TEST_ASSERT_TRUE(store.persistent());
    TEST_ASSERT_EQUAL(ESP_OK, store.initialize());
    TEST_ASSERT_EQUAL_size_t(2, fake_platform::nvs_init_call_count());
}

}  // namespace

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_memory_initialization_retries_after_nvs_failure);
    RUN_TEST(test_memory_initialization_recovers_from_callback_mutex_failure);
    RUN_TEST(test_callback_clear_waits_and_blocks_later_callbacks);
    RUN_TEST(test_preferences_initialization_retries_after_nvs_failure);
    return UNITY_END();
}
