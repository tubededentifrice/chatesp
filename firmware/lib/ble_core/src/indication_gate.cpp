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
    if (waiting_.occupied && settings_confirmation &&
        !waiting_.settings_confirmation) {
        assign(
            waiting_, connection_handle, kind, data, size,
            settings_confirmation);
        settings_confirmation_pending_ = true;
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
    settings_confirmation_pending_ =
        settings_confirmation_pending_ || settings_confirmation;
    return true;
}

bool IndicationGate::pending(
    std::uint16_t &connection_handle,
    const std::uint8_t *&data,
    std::size_t &size) const {
    const Slot *slot = pending_slot();
    if (slot == nullptr) {
        return false;
    }
    connection_handle = slot->connection_handle;
    data = slot->data.data();
    size = slot->size;
    return true;
}

bool IndicationGate::begin_attempt() {
    Slot *slot = pending_slot();
    if (slot == nullptr || slot->attempts >= kMaximumAttempts) {
        return false;
    }
    ++slot->attempts;
    return true;
}

bool IndicationGate::mark_sent() {
    if (active_.occupied && !active_.in_flight) {
        active_.in_flight = true;
        return true;
    }
    if (active_.occupied || !waiting_.occupied || waiting_.attempts == 0) {
        return false;
    }
    active_ = waiting_;
    active_.in_flight = true;
    clear(waiting_);
    return true;
}

IndicationDeliveryAction IndicationGate::send_failed() {
    Slot *slot = pending_slot();
    if (slot == nullptr || slot->attempts == 0) {
        return IndicationDeliveryAction::none;
    }
    return failed_action(*slot);
}

IndicationDeliveryAction IndicationGate::complete(bool confirmed) {
    if (!active_.occupied || !active_.in_flight) {
        return IndicationDeliveryAction::none;
    }
    if (!confirmed) {
        active_.in_flight = false;
        return failed_action(active_);
    }
    const bool settings_confirmation = active_.settings_confirmation;
    clear(active_);
    if (settings_confirmation) {
        settings_confirmation_pending_ =
            waiting_.occupied && waiting_.settings_confirmation;
        return IndicationDeliveryAction::settings_confirmed;
    }
    return IndicationDeliveryAction::complete;
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

bool IndicationGate::waiting() const { return pending_slot() != nullptr; }

bool IndicationGate::in_flight() const {
    return active_.occupied && active_.in_flight;
}

bool IndicationGate::settings_confirmation_pending() const {
    return settings_confirmation_pending_;
}

void IndicationGate::reset_delivery() {
    clear(active_);
    clear(waiting_);
}

void IndicationGate::reset() {
    reset_delivery();
    settings_confirmation_pending_ = false;
}

IndicationGate::Slot *IndicationGate::pending_slot() {
    if (active_.occupied && !active_.in_flight) {
        return &active_;
    }
    return waiting_.occupied ? &waiting_ : nullptr;
}

const IndicationGate::Slot *IndicationGate::pending_slot() const {
    if (active_.occupied && !active_.in_flight) {
        return &active_;
    }
    return waiting_.occupied ? &waiting_ : nullptr;
}

IndicationDeliveryAction IndicationGate::failed_action(Slot &slot) {
    if (slot.attempts < kMaximumAttempts) {
        return slot.settings_confirmation
            ? IndicationDeliveryAction::settings_retry
            : IndicationDeliveryAction::retry;
    }
    const bool settings_confirmation = slot.settings_confirmation;
    clear(slot);
    return settings_confirmation
        ? IndicationDeliveryAction::settings_failed
        : IndicationDeliveryAction::complete;
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
    slot.in_flight = false;
    slot.attempts = 0;
    slot.occupied = true;
}

void IndicationGate::clear(Slot &slot) {
    std::fill(slot.data.begin(), slot.data.end(), 0);
    slot.size = 0;
    slot.connection_handle = 0;
    slot.kind = IndicationKind::settings;
    slot.settings_confirmation = false;
    slot.in_flight = false;
    slot.attempts = 0;
    slot.occupied = false;
}

}  // namespace provisioning
}  // namespace chatesp
