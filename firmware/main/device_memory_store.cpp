#include "device_memory_store.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include "nvs.h"
#include "nvs_flash.h"

namespace chatesp {
namespace {

#ifndef CHATESP_DEVELOPMENT_MODE
#define CHATESP_DEVELOPMENT_MODE 0
#endif

#if CHATESP_DEVELOPMENT_MODE
constexpr char kNamespace[] = "chesp_mem_dev";
#else
constexpr char kNamespace[] = "chesp_mem_prod";
#endif
constexpr char kActiveKey[] = "active";
constexpr char kRecordAKey[] = "record_a";
constexpr char kRecordBKey[] = "record_b";

const char *record_key(std::uint8_t slot) {
    return slot == 1 ? kRecordAKey : kRecordBKey;
}

bool same_fact(
    const agent::MemoryEntry &entry, const char *fact, std::size_t size) {
    return entry.fact.size() == size &&
        std::memcmp(entry.fact.data(), fact, size) == 0;
}

void sort_entries(agent::MemorySnapshot &snapshot) {
    std::sort(
        snapshot.entries.begin(), snapshot.entries.begin() + snapshot.size,
        [](const agent::MemoryEntry &left, const agent::MemoryEntry &right) {
            return left.id < right.id;
        });
}

}  // namespace

DeviceMemoryStore::~DeviceMemoryStore() {
    clear_turn_state();
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
    }
}

bool DeviceMemoryStore::lock() {
    return mutex_ != nullptr &&
        xSemaphoreTake(mutex_, pdMS_TO_TICKS(2'000)) == pdTRUE;
}

void DeviceMemoryStore::unlock() {
    if (mutex_ != nullptr) {
        xSemaphoreGive(mutex_);
    }
}

esp_err_t DeviceMemoryStore::initialize() {
    if (initialized_) {
        return persistent_ ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    initialized_ = true;
    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    snapshot_.clear();
    snapshot_.fingerprint = agent::compute_memory_fingerprint(snapshot_);
    const esp_err_t init_result = nvs_flash_init();
    if (init_result != ESP_OK) {
        return init_result;
    }
    persistent_ = true;
    nvs_handle_t handle = 0;
    const esp_err_t open_result = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (open_result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (open_result != ESP_OK) {
        persistent_ = false;
        return open_result;
    }
    std::uint8_t active_slot = 0;
    esp_err_t result = nvs_get_u8(handle, kActiveKey, &active_slot);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (result != ESP_OK || (active_slot != 1 && active_slot != 2)) {
        nvs_close(handle);
        persistent_ = result == ESP_OK;
        return result == ESP_OK ? ESP_OK : result;
    }
    std::size_t size = 0;
    result = nvs_get_blob(handle, record_key(active_slot), nullptr, &size);
    if (result != ESP_OK || size > agent::max_encoded_memory_record_bytes) {
        nvs_close(handle);
        persistent_ = result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND;
        return result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND
            ? ESP_OK
            : result;
    }
    agent::EncodedMemoryRecord encoded{};
    result = nvs_get_blob(
        handle, record_key(active_slot), encoded.data(), &size);
    nvs_close(handle);
    if (result != ESP_OK) {
        persistent_ = false;
        return result;
    }
    agent::MemorySnapshot decoded;
    if (agent::decode_memory_record(encoded.data(), size, decoded)) {
        snapshot_ = decoded;
        active_slot_ = active_slot;
    }
    return ESP_OK;
}

agent::Error DeviceMemoryStore::snapshot(agent::MemorySnapshot &snapshot) {
    if (!lock()) {
        return agent::Error::tool_failed;
    }
    snapshot = snapshot_;
    unlock();
    return agent::Error::none;
}

void DeviceMemoryStore::fill_result_locked(
    agent::MemoryMutationStatus status,
    agent::MemoryMutationResult &result, std::uint32_t id,
    bool compacted) const {
    result.status = status;
    result.id = id;
    result.revision = snapshot_.revision;
    result.fingerprint = snapshot_.fingerprint;
    result.count = snapshot_.size;
    result.compacted = compacted;
}

bool DeviceMemoryStore::expected_version_locked(
    std::uint32_t revision,
    const agent::MemoryFingerprint &fingerprint) const {
    return revision == snapshot_.revision &&
        agent::memory_fingerprints_equal(fingerprint, snapshot_.fingerprint);
}

bool DeviceMemoryStore::persist_locked(
    const agent::MemorySnapshot &next) {
    if (!persistent_) {
        return false;
    }
    agent::EncodedMemoryRecord encoded{};
    std::size_t encoded_size = 0;
    if (!agent::encode_memory_record(next, encoded, encoded_size)) {
        return false;
    }
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    const std::uint8_t next_slot = active_slot_ == 1 ? 2 : 1;
    esp_err_t result = nvs_set_blob(
        handle, record_key(next_slot), encoded.data(), encoded_size);
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    agent::EncodedMemoryRecord readback{};
    std::size_t readback_size = readback.size();
    if (result == ESP_OK) {
        result = nvs_get_blob(
            handle, record_key(next_slot), readback.data(), &readback_size);
    }
    if (result != ESP_OK || readback_size != encoded_size ||
        std::memcmp(encoded.data(), readback.data(), encoded_size) != 0) {
        nvs_close(handle);
        return false;
    }
    agent::MemorySnapshot decoded;
    if (!agent::decode_memory_record(
            readback.data(), readback_size, decoded) ||
        decoded.revision != next.revision ||
        !agent::memory_fingerprints_equal(
            decoded.fingerprint, next.fingerprint)) {
        nvs_close(handle);
        return false;
    }
    result = nvs_set_u8(handle, kActiveKey, next_slot);
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    if (result != ESP_OK) {
        return false;
    }
    snapshot_ = decoded;
    active_slot_ = next_slot;
    return true;
}

void DeviceMemoryStore::notify_change(
    const agent::MemorySnapshot &snapshot) {
    if (change_callback_ != nullptr) {
        change_callback_(snapshot, change_context_);
    }
}

agent::Error DeviceMemoryStore::remember_locked(
    const char *fact, std::size_t size, bool keep_pending_when_full,
    agent::MemoryMutationResult &result,
    agent::MemorySnapshot &changed_snapshot, bool &changed) {
    changed = false;
    if (!agent::valid_memory_fact(fact, size)) {
        fill_result_locked(agent::MemoryMutationStatus::invalid_field, result);
        return agent::Error::none;
    }
    for (std::size_t index = 0; index < snapshot_.size; ++index) {
        if (same_fact(snapshot_.entries[index], fact, size)) {
            pending_fact_.clear();
            fill_result_locked(
                agent::MemoryMutationStatus::unchanged, result,
                snapshot_.entries[index].id);
            return agent::Error::none;
        }
    }
    if (snapshot_.size == agent::Limits::max_memory_facts) {
        if (keep_pending_when_full) {
            pending_fact_.assign(fact, size);
        }
        fill_result_locked(agent::MemoryMutationStatus::full, result);
        return agent::Error::none;
    }
    if (snapshot_.revision == std::numeric_limits<std::uint32_t>::max() ||
        snapshot_.next_id == std::numeric_limits<std::uint32_t>::max()) {
        fill_result_locked(agent::MemoryMutationStatus::storage_failure, result);
        return agent::Error::none;
    }
    agent::MemorySnapshot next = snapshot_;
    agent::MemoryEntry &entry = next.entries[next.size++];
    entry.id = next.next_id++;
    if (!entry.fact.assign(fact, size)) {
        fill_result_locked(agent::MemoryMutationStatus::invalid_field, result);
        return agent::Error::none;
    }
    ++next.revision;
    next.fingerprint = agent::compute_memory_fingerprint(next);
    if (!persist_locked(next)) {
        fill_result_locked(agent::MemoryMutationStatus::storage_failure, result);
        return agent::Error::none;
    }
    pending_fact_.clear();
    fill_result_locked(
        agent::MemoryMutationStatus::applied, result, entry.id);
    changed_snapshot = snapshot_;
    changed = true;
    return agent::Error::none;
}

agent::Error DeviceMemoryStore::remember(
    const char *fact, std::size_t size,
    agent::MemoryMutationResult &result) {
    if (!lock()) return agent::Error::tool_failed;
    agent::MemorySnapshot changed_snapshot;
    bool changed = false;
    const agent::Error error = remember_locked(
        fact, size, true, result, changed_snapshot, changed);
    unlock();
    if (changed) notify_change(changed_snapshot);
    return error;
}

agent::Error DeviceMemoryStore::forget_locked(
    std::uint32_t id, agent::MemoryMutationResult &result,
    agent::MemorySnapshot &changed_snapshot, bool &changed) {
    changed = false;
    std::size_t found = snapshot_.size;
    for (std::size_t index = 0; index < snapshot_.size; ++index) {
        if (snapshot_.entries[index].id == id) {
            found = index;
            break;
        }
    }
    if (found == snapshot_.size) {
        fill_result_locked(agent::MemoryMutationStatus::not_found, result, id);
        return agent::Error::none;
    }
    if (snapshot_.revision == std::numeric_limits<std::uint32_t>::max()) {
        fill_result_locked(agent::MemoryMutationStatus::storage_failure, result, id);
        return agent::Error::none;
    }
    agent::MemorySnapshot next = snapshot_;
    for (std::size_t index = found + 1; index < next.size; ++index) {
        next.entries[index - 1] = next.entries[index];
    }
    next.entries[next.size - 1].clear();
    --next.size;
    ++next.revision;
    next.fingerprint = agent::compute_memory_fingerprint(next);
    if (!persist_locked(next)) {
        fill_result_locked(agent::MemoryMutationStatus::storage_failure, result, id);
        return agent::Error::none;
    }
    pending_fact_.clear();
    fill_result_locked(agent::MemoryMutationStatus::applied, result, id);
    changed_snapshot = snapshot_;
    changed = true;
    return agent::Error::none;
}

agent::Error DeviceMemoryStore::forget(
    std::uint32_t id, agent::MemoryMutationResult &result) {
    if (!lock()) return agent::Error::tool_failed;
    agent::MemorySnapshot changed_snapshot;
    bool changed = false;
    const agent::Error error = forget_locked(
        id, result, changed_snapshot, changed);
    unlock();
    if (changed) notify_change(changed_snapshot);
    return error;
}

agent::Error DeviceMemoryStore::clear_locked(
    agent::MemoryMutationResult &result,
    agent::MemorySnapshot &changed_snapshot, bool &changed) {
    changed = false;
    if (snapshot_.size == 0) {
        pending_fact_.clear();
        fill_result_locked(agent::MemoryMutationStatus::unchanged, result);
        return agent::Error::none;
    }
    if (snapshot_.revision == std::numeric_limits<std::uint32_t>::max()) {
        fill_result_locked(agent::MemoryMutationStatus::storage_failure, result);
        return agent::Error::none;
    }
    agent::MemorySnapshot next;
    next.clear();
    next.revision = snapshot_.revision + 1;
    next.next_id = snapshot_.next_id;
    next.fingerprint = agent::compute_memory_fingerprint(next);
    if (!persist_locked(next)) {
        fill_result_locked(agent::MemoryMutationStatus::storage_failure, result);
        return agent::Error::none;
    }
    pending_fact_.clear();
    fill_result_locked(agent::MemoryMutationStatus::applied, result);
    changed_snapshot = snapshot_;
    changed = true;
    return agent::Error::none;
}

agent::Error DeviceMemoryStore::clear_memories(
    agent::MemoryMutationResult &result) {
    if (!lock()) return agent::Error::tool_failed;
    agent::MemorySnapshot changed_snapshot;
    bool changed = false;
    const agent::Error error = clear_locked(
        result, changed_snapshot, changed);
    unlock();
    if (changed) notify_change(changed_snapshot);
    return error;
}

agent::Error DeviceMemoryStore::compact(
    const agent::MemoryCompactionPlan &plan,
    agent::MemoryMutationResult &result) {
    if (!lock()) return agent::Error::tool_failed;
    agent::MemorySnapshot changed_snapshot;
    bool changed = false;
    const auto finish = [&](agent::MemoryMutationStatus status) {
        fill_result_locked(status, result, 0, status == agent::MemoryMutationStatus::applied);
        unlock();
        if (changed) notify_change(changed_snapshot);
        return agent::Error::none;
    };
    if (plan.size > agent::Limits::max_memory_facts ||
        (plan.include_pending && pending_fact_.empty()) ||
        (!plan.include_pending && !pending_fact_.empty())) {
        pending_fact_.clear();
        return finish(agent::MemoryMutationStatus::invalid_field);
    }
    std::array<bool, agent::Limits::max_memory_facts> used{};
    agent::MemorySnapshot next;
    next.clear();
    next.revision = snapshot_.revision;
    next.next_id = snapshot_.next_id;
    for (std::size_t output_index = 0; output_index < plan.size; ++output_index) {
        const agent::MemoryCompactionEntry &requested = plan.entries[output_index];
        if (requested.source_count == 0 ||
            !agent::valid_memory_fact(requested.fact.data(), requested.fact.size())) {
            pending_fact_.clear();
            return finish(agent::MemoryMutationStatus::invalid_field);
        }
        std::size_t single_source_index = snapshot_.size;
        for (std::size_t source = 0; source < requested.source_count; ++source) {
            std::size_t found = snapshot_.size;
            for (std::size_t index = 0; index < snapshot_.size; ++index) {
                if (snapshot_.entries[index].id == requested.source_ids[source]) {
                    found = index;
                    break;
                }
            }
            if (found == snapshot_.size || used[found]) {
                pending_fact_.clear();
                return finish(agent::MemoryMutationStatus::invalid_field);
            }
            used[found] = true;
            single_source_index = found;
        }
        agent::MemoryEntry &entry = next.entries[next.size++];
        const bool unchanged = requested.source_count == 1 &&
            same_fact(snapshot_.entries[single_source_index],
                      requested.fact.data(), requested.fact.size());
        if (unchanged) {
            entry.id = snapshot_.entries[single_source_index].id;
        } else {
            if (next.next_id == std::numeric_limits<std::uint32_t>::max()) {
                pending_fact_.clear();
                return finish(agent::MemoryMutationStatus::storage_failure);
            }
            entry.id = next.next_id++;
        }
        entry.fact = requested.fact;
    }
    if (plan.include_pending) {
        bool duplicate = false;
        for (std::size_t index = 0; index < next.size; ++index) {
            duplicate = duplicate || same_fact(
                next.entries[index], pending_fact_.data(), pending_fact_.size());
        }
        if (!duplicate) {
            if (next.size == agent::Limits::max_memory_facts ||
                next.next_id == std::numeric_limits<std::uint32_t>::max()) {
                pending_fact_.clear();
                return finish(
                    next.next_id == std::numeric_limits<std::uint32_t>::max()
                        ? agent::MemoryMutationStatus::storage_failure
                        : agent::MemoryMutationStatus::invalid_field);
            }
            agent::MemoryEntry &entry = next.entries[next.size++];
            entry.id = next.next_id++;
            entry.fact = pending_fact_;
        }
    }
    sort_entries(next);
    for (std::size_t left = 0; left < next.size; ++left) {
        for (std::size_t right = left + 1; right < next.size; ++right) {
            if (same_fact(next.entries[left], next.entries[right].fact.data(),
                          next.entries[right].fact.size())) {
                pending_fact_.clear();
                return finish(agent::MemoryMutationStatus::invalid_field);
            }
        }
    }
    const bool smaller = next.size < snapshot_.size ||
        next.total_fact_bytes() < snapshot_.total_fact_bytes();
    if ((!plan.include_pending && !smaller) ||
        (plan.include_pending && plan.size >= agent::Limits::max_memory_facts)) {
        pending_fact_.clear();
        return finish(agent::MemoryMutationStatus::invalid_field);
    }
    if (snapshot_.revision == std::numeric_limits<std::uint32_t>::max()) {
        pending_fact_.clear();
        return finish(agent::MemoryMutationStatus::storage_failure);
    }
    ++next.revision;
    next.fingerprint = agent::compute_memory_fingerprint(next);
    if (next.size == snapshot_.size &&
        agent::memory_fingerprints_equal(next.fingerprint, snapshot_.fingerprint)) {
        pending_fact_.clear();
        return finish(agent::MemoryMutationStatus::unchanged);
    }
    if (!persist_locked(next)) {
        pending_fact_.clear();
        return finish(agent::MemoryMutationStatus::storage_failure);
    }
    pending_fact_.clear();
    changed_snapshot = snapshot_;
    changed = true;
    return finish(agent::MemoryMutationStatus::applied);
}

void DeviceMemoryStore::clear_turn_state() {
    if (mutex_ == nullptr || !lock()) return;
    pending_fact_.clear();
    unlock();
}

agent::Error DeviceMemoryStore::add_from_ble(
    const char *fact, std::size_t size, std::uint32_t expected_revision,
    const agent::MemoryFingerprint &expected_fingerprint,
    agent::MemoryMutationResult &result) {
    if (!lock()) return agent::Error::tool_failed;
    if (!expected_version_locked(expected_revision, expected_fingerprint)) {
        fill_result_locked(agent::MemoryMutationStatus::revision_conflict, result);
        unlock();
        return agent::Error::none;
    }
    agent::MemorySnapshot changed_snapshot;
    bool changed = false;
    const agent::Error error = remember_locked(
        fact, size, false, result, changed_snapshot, changed);
    unlock();
    if (changed) notify_change(changed_snapshot);
    return error;
}

agent::Error DeviceMemoryStore::forget_from_ble(
    std::uint32_t id, std::uint32_t expected_revision,
    const agent::MemoryFingerprint &expected_fingerprint,
    agent::MemoryMutationResult &result) {
    if (!lock()) return agent::Error::tool_failed;
    if (!expected_version_locked(expected_revision, expected_fingerprint)) {
        fill_result_locked(agent::MemoryMutationStatus::revision_conflict, result);
        unlock();
        return agent::Error::none;
    }
    agent::MemorySnapshot changed_snapshot;
    bool changed = false;
    const agent::Error error = forget_locked(
        id, result, changed_snapshot, changed);
    unlock();
    if (changed) notify_change(changed_snapshot);
    return error;
}

agent::Error DeviceMemoryStore::clear_from_ble(
    std::uint32_t expected_revision,
    const agent::MemoryFingerprint &expected_fingerprint,
    agent::MemoryMutationResult &result) {
    if (!lock()) return agent::Error::tool_failed;
    if (!expected_version_locked(expected_revision, expected_fingerprint)) {
        fill_result_locked(agent::MemoryMutationStatus::revision_conflict, result);
        unlock();
        return agent::Error::none;
    }
    agent::MemorySnapshot changed_snapshot;
    bool changed = false;
    const agent::Error error = clear_locked(
        result, changed_snapshot, changed);
    unlock();
    if (changed) notify_change(changed_snapshot);
    return error;
}

void DeviceMemoryStore::set_change_callback(
    ChangeCallback callback, void *context) {
    if (!lock()) return;
    change_callback_ = callback;
    change_context_ = context;
    unlock();
}

}  // namespace chatesp
