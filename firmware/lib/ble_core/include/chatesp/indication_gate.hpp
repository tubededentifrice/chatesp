#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace chatesp {
namespace provisioning {

enum class IndicationKind : std::uint8_t {
    settings,
    device_context,
};

enum class IndicationDeliveryAction : std::uint8_t {
    none,
    complete,
    retry,
    settings_retry,
    settings_confirmed,
    settings_failed,
};

class IndicationGate {
public:
    static constexpr std::size_t kMaximumSize = 48;
    static constexpr std::uint8_t kMaximumAttempts = 3;

    [[nodiscard]] bool enqueue(
        std::uint16_t connection_handle,
        IndicationKind kind,
        const std::uint8_t *data,
        std::size_t size,
        bool settings_confirmation);
    [[nodiscard]] bool pending(
        std::uint16_t &connection_handle,
        const std::uint8_t *&data,
        std::size_t &size) const;
    [[nodiscard]] bool begin_attempt();
    [[nodiscard]] bool mark_sent();
    [[nodiscard]] IndicationDeliveryAction send_failed();
    [[nodiscard]] IndicationDeliveryAction complete(bool confirmed);
    [[nodiscard]] bool clear_connection(std::uint16_t connection_handle);

    [[nodiscard]] bool waiting() const;
    [[nodiscard]] bool in_flight() const;
    [[nodiscard]] bool settings_confirmation_pending() const;
    void reset_delivery();
    void reset();

private:
    struct Slot {
        std::array<std::uint8_t, kMaximumSize> data{};
        std::size_t size = 0;
        std::uint16_t connection_handle = 0;
        IndicationKind kind = IndicationKind::settings;
        bool settings_confirmation = false;
        bool in_flight = false;
        std::uint8_t attempts = 0;
        bool occupied = false;
    };

    [[nodiscard]] Slot *pending_slot();
    [[nodiscard]] const Slot *pending_slot() const;
    [[nodiscard]] IndicationDeliveryAction failed_action(Slot &slot);

    static bool matches(
        const Slot &slot,
        std::uint16_t connection_handle,
        IndicationKind kind,
        const std::uint8_t *data,
        std::size_t size);
    static void assign(
        Slot &slot,
        std::uint16_t connection_handle,
        IndicationKind kind,
        const std::uint8_t *data,
        std::size_t size,
        bool settings_confirmation);
    static void clear(Slot &slot);

    Slot active_{};
    Slot waiting_{};
    bool settings_confirmation_pending_ = false;
};

}  // namespace provisioning
}  // namespace chatesp
