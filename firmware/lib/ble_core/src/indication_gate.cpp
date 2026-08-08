#include "chatesp/indication_gate.hpp"

#include <algorithm>
#include <cstring>

namespace chatesp {
namespace provisioning {

bool IndicationGate::enqueue(
    std::uint16_t connection_handle,
    IndicationKind kind,
    const std::uint8_t *data,
    std::size_t size,
    bool settings_confirmation) {
    if (data == nullptr || size == 0 || size > kMaximumSize) {
        return false;
    }
    if (matches(active_, connection_handle, kind, data, size) ||
        matches(waiting_, connection_handle, kind, data, size)) {
        return true;
    }
    if (waiting_.occupied &&
        (kind != IndicationKind::settings ||
         waiting_.kind == IndicationKind::settings)) {
        return false;
    }
    assign(
        waiting_, connection_handle, kind, data, size,
        settings_confirmation);
    return true;
}

bool IndicationGate::pending(
    std::uint16_t &connection_handle,
    const std::uint8_t *&data,
    std::size_t &size) const {
    if (!waiting_.occupied) {
        return false;
    }
    connection_handle = waiting_.connection_handle;
    data = waiting_.data.data();
    size = waiting_.size;
    return true;
}

bool IndicationGate::mark_sent() {
    if (active_.occupied || !waiting_.occupied) {
        return false;
    }
    active_ = waiting_;
    clear(waiting_);
    return true;
}

bool IndicationGate::complete() {
    if (!active_.occupied) {
        return false;
    }
    const bool was_settings_confirmation = active_.settings_confirmation;
    clear(active_);
    return was_settings_confirmation;
}

bool IndicationGate::clear_connection(std::uint16_t connection_handle) {
    bool settings_confirmation = false;
    if (active_.occupied && active_.connection_handle == connection_handle) {
        settings_confirmation = active_.settings_confirmation;
        clear(active_);
    }
    if (waiting_.occupied && waiting_.connection_handle == connection_handle) {
        settings_confirmation =
            settings_confirmation || waiting_.settings_confirmation;
        clear(waiting_);
    }
    return settings_confirmation;
}

bool IndicationGate::waiting() const { return waiting_.occupied; }

bool IndicationGate::in_flight() const { return active_.occupied; }

bool IndicationGate::settings_confirmation_pending() const {
    return (active_.occupied && active_.settings_confirmation) ||
        (waiting_.occupied && waiting_.settings_confirmation);
}

void IndicationGate::reset() {
    clear(active_);
    clear(waiting_);
}

bool IndicationGate::matches(
    const Slot &slot,
    std::uint16_t connection_handle,
    IndicationKind kind,
    const std::uint8_t *data,
    std::size_t size) {
    return slot.occupied && slot.connection_handle == connection_handle &&
        slot.kind == kind && slot.size == size &&
        std::memcmp(slot.data.data(), data, size) == 0;
}

void IndicationGate::assign(
    Slot &slot,
    std::uint16_t connection_handle,
    IndicationKind kind,
    const std::uint8_t *data,
    std::size_t size,
    bool settings_confirmation) {
    clear(slot);
    std::copy_n(data, size, slot.data.begin());
    slot.size = size;
    slot.connection_handle = connection_handle;
    slot.kind = kind;
    slot.settings_confirmation = settings_confirmation;
    slot.occupied = true;
}

void IndicationGate::clear(Slot &slot) {
    std::fill(slot.data.begin(), slot.data.end(), 0);
    slot.size = 0;
    slot.connection_handle = 0;
    slot.kind = IndicationKind::settings;
    slot.settings_confirmation = false;
    slot.occupied = false;
}

}  // namespace provisioning
}  // namespace chatesp
