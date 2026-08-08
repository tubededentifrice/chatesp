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

class IndicationGate {
public:
    static constexpr std::size_t kMaximumSize = 48;

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
    [[nodiscard]] bool mark_sent();
    [[nodiscard]] bool complete();
    [[nodiscard]] bool clear_connection(std::uint16_t connection_handle);

    [[nodiscard]] bool waiting() const;
    [[nodiscard]] bool in_flight() const;
    [[nodiscard]] bool settings_confirmation_pending() const;
    void reset();

private:
    struct Slot {
        std::array<std::uint8_t, kMaximumSize> data{};
        std::size_t size = 0;
        std::uint16_t connection_handle = 0;
        IndicationKind kind = IndicationKind::settings;
        bool settings_confirmation = false;
        bool occupied = false;
    };

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
};

}  // namespace provisioning
}  // namespace chatesp
