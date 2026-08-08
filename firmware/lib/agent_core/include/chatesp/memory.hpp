#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "chatesp/agent_limits.hpp"
#include "chatesp/fixed_text.hpp"

namespace chatesp {
namespace agent {

using MemoryFingerprint = std::array<std::uint8_t, 32>;

struct MemoryEntry {
    std::uint32_t id = 0;
    FixedText<Limits::max_memory_fact_bytes> fact;

    void clear() {
        id = 0;
        fact.clear();
    }
};

struct MemorySnapshot {
    std::uint32_t revision = 0;
    std::uint32_t next_id = 1;
    MemoryFingerprint fingerprint{};
    std::array<MemoryEntry, Limits::max_memory_facts> entries{};
    std::size_t size = 0;

    void clear();
    [[nodiscard]] const MemoryEntry *find(std::uint32_t id) const;
    [[nodiscard]] std::size_t total_fact_bytes() const;
};

enum class MemoryMutationStatus : std::uint8_t {
    applied,
    unchanged,
    full,
    not_found,
    revision_conflict,
    invalid_field,
    storage_failure,
};

struct MemoryMutationResult {
    MemoryMutationStatus status = MemoryMutationStatus::invalid_field;
    std::uint32_t id = 0;
    std::uint32_t revision = 0;
    MemoryFingerprint fingerprint{};
    std::size_t count = 0;
    bool compacted = false;
};

struct MemoryCompactionEntry {
    std::array<std::uint32_t, Limits::max_memory_facts> source_ids{};
    std::size_t source_count = 0;
    FixedText<Limits::max_memory_fact_bytes> fact;

    void clear();
};

struct MemoryCompactionPlan {
    std::array<MemoryCompactionEntry, Limits::max_memory_facts> entries{};
    std::size_t size = 0;
    bool include_pending = false;

    void clear();
};

static constexpr std::size_t memory_record_header_bytes = 48;
static constexpr std::size_t memory_record_checksum_bytes = 32;
static constexpr std::size_t memory_record_entry_header_bytes = 6;
static constexpr std::size_t max_encoded_memory_record_bytes =
    memory_record_header_bytes + memory_record_checksum_bytes +
    Limits::max_memory_facts *
        (memory_record_entry_header_bytes + Limits::max_memory_fact_bytes);
using EncodedMemoryRecord =
    std::array<std::uint8_t, max_encoded_memory_record_bytes>;

[[nodiscard]] bool valid_memory_fact(const char *text, std::size_t size);
[[nodiscard]] MemoryFingerprint compute_memory_fingerprint(
    const MemorySnapshot &snapshot);
[[nodiscard]] bool memory_fingerprints_equal(
    const MemoryFingerprint &left, const MemoryFingerprint &right);
[[nodiscard]] bool encode_memory_record(
    const MemorySnapshot &snapshot, EncodedMemoryRecord &encoded,
    std::size_t &encoded_size);
[[nodiscard]] bool decode_memory_record(
    const std::uint8_t *encoded, std::size_t encoded_size,
    MemorySnapshot &snapshot);

enum class MemoryBleOperation : std::uint8_t {
    changed = 0,
    list_page = 1,
    add = 2,
    forget = 3,
    clear = 4,
};

enum class MemoryBleStatus : std::uint8_t {
    applied = 0x00,
    unchanged = 0x01,
    full = 0x02,
    not_found = 0x03,
    revision_conflict = 0x04,
    invalid_field = 0x10,
    storage_failure = 0x11,
    authentication_required = 0x12,
    busy = 0x13,
    unsupported_version = 0x14,
};

static constexpr std::uint8_t memory_ble_version = 1;
static constexpr std::size_t memory_ble_command_header_bytes = 54;
static constexpr std::size_t memory_ble_response_header_bytes = 56;
static constexpr std::size_t max_memory_ble_command_bytes =
    memory_ble_command_header_bytes + Limits::max_memory_fact_bytes;
static constexpr std::size_t max_memory_ble_response_bytes =
    memory_ble_response_header_bytes + Limits::max_memory_fact_bytes;

struct MemoryBleCommand {
    MemoryBleOperation operation = MemoryBleOperation::list_page;
    std::uint32_t request_id = 0;
    std::uint32_t expected_revision = 0;
    MemoryFingerprint expected_fingerprint{};
    std::uint32_t memory_id = 0;
    FixedText<Limits::max_memory_fact_bytes> fact;
};

struct MemoryBleResponse {
    MemoryBleStatus status = MemoryBleStatus::invalid_field;
    MemoryBleOperation operation = MemoryBleOperation::list_page;
    std::uint32_t request_id = 0;
    std::uint32_t revision = 0;
    MemoryFingerprint fingerprint{};
    std::uint32_t memory_id = 0;
    std::uint8_t total_count = 0;
    bool has_item = false;
    bool has_more = false;
    bool changed_event = false;
    FixedText<Limits::max_memory_fact_bytes> fact;
};

[[nodiscard]] MemoryBleStatus parse_memory_ble_command(
    const std::uint8_t *data, std::size_t size, bool link_secure,
    MemoryBleCommand &command);
[[nodiscard]] bool encode_memory_ble_response(
    const MemoryBleResponse &response,
    std::array<std::uint8_t, max_memory_ble_response_bytes> &encoded,
    std::size_t &encoded_size);

}  // namespace agent
}  // namespace chatesp
