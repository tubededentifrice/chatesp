#include "chatesp/crash_trace.hpp"

#include <cstring>

namespace chatesp {
namespace runtime {
namespace {

constexpr std::uint32_t kMagic = 0x43525443U;
constexpr std::uint16_t kVersion = 3;

std::uint32_t checksum(const CrashBootRecord &boot) {
    constexpr std::uint32_t kOffset = 2'166'136'261U;
    constexpr std::uint32_t kPrime = 16'777'619U;
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&boot);
    std::uint32_t value = kOffset;
    for (std::size_t index = 0;
         index < offsetof(CrashBootRecord, last_heartbeat_ms); ++index) {
        value ^= bytes[index];
        value *= kPrime;
    }
    return value;
}

void update_checksum(CrashBootRecord &boot) {
    boot.checksum = checksum(boot);
}

bool header_valid(const CrashTraceStore &store) {
    return store.magic == kMagic && store.version == kVersion &&
        store.reserved == 0;
}

}  // namespace

bool crash_trace_valid(const CrashTraceStore &store) {
    if (!header_valid(store)) {
        return false;
    }
    for (const CrashBootRecord &boot : store.boots) {
        if (boot.sequence != 0 && !crash_boot_record_valid(boot)) {
            return false;
        }
    }
    return true;
}

bool crash_boot_record_valid(const CrashBootRecord &boot) {
    return boot.sequence != 0 && boot.active <= 1 &&
        boot.event_count <= boot.events.size() &&
        boot.next_event < boot.events.size() &&
        boot.checksum == checksum(boot);
}

std::size_t crash_trace_active_index(const CrashTraceStore &store) {
    std::size_t result = store.boots.size();
    std::uint32_t newest_sequence = 0;
    if (!header_valid(store)) {
        return result;
    }
    for (std::size_t index = 0; index < store.boots.size(); ++index) {
        const CrashBootRecord &boot = store.boots[index];
        if (crash_boot_record_valid(boot) && boot.active != 0 &&
            (result == store.boots.size() ||
             boot.sequence > newest_sequence)) {
            result = index;
            newest_sequence = boot.sequence;
        }
    }
    return result;
}

void crash_trace_begin_boot(
    CrashTraceStore &store, std::uint32_t reset_reason) {
    if (!header_valid(store)) {
        std::memset(&store, 0, sizeof(store));
        store.magic = kMagic;
        store.version = kVersion;
    }

    std::size_t newest_index = store.boots.size();
    std::uint32_t newest_sequence = 0;
    for (std::size_t index = 0; index < store.boots.size(); ++index) {
        const CrashBootRecord &boot = store.boots[index];
        if (crash_boot_record_valid(boot) &&
            (newest_index == store.boots.size() ||
             boot.sequence > newest_sequence)) {
            newest_index = index;
            newest_sequence = boot.sequence;
        }
    }
    if (newest_index != store.boots.size() &&
        store.boots[newest_index].active != 0) {
        CrashBootRecord completed = store.boots[newest_index];
        completed.active = 0;
        completed.outcome_reset_reason = reset_reason;
        update_checksum(completed);
        store.boots[newest_index] = completed;
    }

    const std::size_t current_index = newest_index == store.boots.size()
        ? 0
        : (newest_index + 1U) % store.boots.size();
    CrashBootRecord current{};
    current.sequence = newest_sequence + 1U;
    if (current.sequence == 0) {
        current.sequence = 1;
    }
    current.start_reset_reason = reset_reason;
    current.active = 1;
    update_checksum(current);
    store.boots[current_index] = current;
}

bool crash_trace_mark(
    CrashTraceStore &store, CrashEvent event, std::uint32_t at_ms) {
    const std::size_t active_index = crash_trace_active_index(store);
    if (active_index == store.boots.size()) {
        return false;
    }
    CrashBootRecord boot = store.boots[active_index];
    CrashEventRecord &record = boot.events[boot.next_event];
    record = {at_ms, event, 0};
    boot.next_event = static_cast<std::uint8_t>(
        (boot.next_event + 1U) % boot.events.size());
    if (boot.event_count < boot.events.size()) {
        ++boot.event_count;
    }
    update_checksum(boot);
    store.boots[active_index] = boot;
    return true;
}

bool crash_trace_heartbeat(CrashTraceStore &store, std::uint32_t at_ms) {
    const std::size_t active_index = crash_trace_active_index(store);
    if (active_index == store.boots.size()) {
        return false;
    }
    // The heartbeat is one independent RTC word. It is excluded from the boot
    // checksum, so a reset during this write cannot invalidate the event ring.
    store.boots[active_index].last_heartbeat_ms = at_ms;
    return true;
}

}  // namespace runtime
}  // namespace chatesp
