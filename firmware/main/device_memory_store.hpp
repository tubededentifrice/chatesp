#pragma once

#include <cstddef>
#include <cstdint>

#include "chatesp/agent_interfaces.hpp"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace chatesp {

class DeviceMemoryStore final : public agent::MemoryControlProvider {
public:
    using ChangeCallback = void (*)(const agent::MemorySnapshot &, void *);

    ~DeviceMemoryStore() override;

    esp_err_t initialize();
    agent::Error snapshot(agent::MemorySnapshot &snapshot) override;
    agent::Error remember(
        const char *fact, std::size_t size,
        agent::MemoryMutationResult &result) override;
    agent::Error forget(
        std::uint32_t id, agent::MemoryMutationResult &result) override;
    agent::Error clear_memories(
        agent::MemoryMutationResult &result) override;
    agent::Error compact(
        const agent::MemoryCompactionPlan &plan,
        agent::MemoryMutationResult &result) override;
    void clear_turn_state() override;

    agent::Error add_from_ble(
        const char *fact, std::size_t size, std::uint32_t expected_revision,
        const agent::MemoryFingerprint &expected_fingerprint,
        agent::MemoryMutationResult &result);
    agent::Error forget_from_ble(
        std::uint32_t id, std::uint32_t expected_revision,
        const agent::MemoryFingerprint &expected_fingerprint,
        agent::MemoryMutationResult &result);
    agent::Error clear_from_ble(
        std::uint32_t expected_revision,
        const agent::MemoryFingerprint &expected_fingerprint,
        agent::MemoryMutationResult &result);
    void set_change_callback(ChangeCallback callback, void *context);

private:
    bool lock();
    void unlock();
    void fill_result_locked(
        agent::MemoryMutationStatus status,
        agent::MemoryMutationResult &result, std::uint32_t id = 0,
        bool compacted = false) const;
    bool expected_version_locked(
        std::uint32_t revision,
        const agent::MemoryFingerprint &fingerprint) const;
    bool persist_locked(const agent::MemorySnapshot &next);
    void notify_change(const agent::MemorySnapshot &snapshot);
    agent::Error remember_locked(
        const char *fact, std::size_t size, bool keep_pending_when_full,
        agent::MemoryMutationResult &result,
        agent::MemorySnapshot &changed_snapshot, bool &changed);
    agent::Error forget_locked(
        std::uint32_t id, agent::MemoryMutationResult &result,
        agent::MemorySnapshot &changed_snapshot, bool &changed);
    agent::Error clear_locked(
        agent::MemoryMutationResult &result,
        agent::MemorySnapshot &changed_snapshot, bool &changed);

    agent::MemorySnapshot snapshot_{};
    agent::FixedText<agent::Limits::max_memory_fact_bytes> pending_fact_;
    SemaphoreHandle_t mutex_ = nullptr;
    ChangeCallback change_callback_ = nullptr;
    void *change_context_ = nullptr;
    bool initialized_ = false;
    bool persistent_ = false;
    std::uint8_t active_slot_ = 0;
};

}  // namespace chatesp
