#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace chatesp {
namespace runtime {

enum class BrightnessOutcome : std::uint8_t {
    unknown,
    pending,
    succeeded,
    failed,
    cancelled,
    superseded,
};

enum class BrightnessCancelResult : std::uint8_t {
    not_found,
    cancelled_before_execution,
    already_executing,
    already_complete,
};

struct BrightnessRequest {
    std::uint32_t generation = 0;
    std::uint8_t percent = 0;

    [[nodiscard]] bool valid() const { return generation != 0; }
};

struct BrightnessSubmission {
    BrightnessRequest request{};
    bool coalesced = false;
};

// The caller must serialize access to this bounded coordinator. One request
// can execute while one newer preview waits. A newer waiting preview replaces
// only the older waiting preview. It never replaces work that already started.
class BrightnessCoordinator {
public:
    [[nodiscard]] BrightnessSubmission submit(std::uint8_t percent) {
        bool coalesced = false;
        if (pending_.valid()) {
            superseded_ = {
                pending_.generation, BrightnessOutcome::superseded};
            coalesced = true;
        }
        ++generation_;
        if (generation_ == 0) {
            ++generation_;
        }
        pending_ = {generation_, percent};
        return {pending_, coalesced};
    }

    [[nodiscard]] BrightnessRequest begin_next() {
        if (active_.valid() || !pending_.valid()) {
            return {};
        }
        active_ = pending_;
        pending_ = {};
        return active_;
    }

    [[nodiscard]] bool complete(
        BrightnessRequest request, bool succeeded) {
        if (!request.valid() || request.generation != active_.generation) {
            return false;
        }
        active_ = {};
        completed_[completed_index_] = {
            request.generation,
            succeeded ? BrightnessOutcome::succeeded
                      : BrightnessOutcome::failed};
        completed_index_ = (completed_index_ + 1) % completed_.size();
        return true;
    }

    [[nodiscard]] BrightnessCancelResult cancel(std::uint32_t generation) {
        if (generation == 0) {
            return BrightnessCancelResult::not_found;
        }
        if (pending_.generation == generation) {
            pending_ = {};
            cancelled_ = {generation, BrightnessOutcome::cancelled};
            return BrightnessCancelResult::cancelled_before_execution;
        }
        if (active_.generation == generation) {
            return BrightnessCancelResult::already_executing;
        }
        const BrightnessOutcome current = outcome(generation);
        return current == BrightnessOutcome::unknown
            ? BrightnessCancelResult::not_found
            : BrightnessCancelResult::already_complete;
    }

    [[nodiscard]] BrightnessRequest cancel_pending() {
        const BrightnessRequest cancelled = pending_;
        if (cancelled.valid()) {
            pending_ = {};
            cancelled_ = {
                cancelled.generation, BrightnessOutcome::cancelled};
        }
        return cancelled;
    }

    [[nodiscard]] BrightnessOutcome outcome(std::uint32_t generation) const {
        if (generation == 0) {
            return BrightnessOutcome::unknown;
        }
        if (pending_.generation == generation ||
            active_.generation == generation) {
            return BrightnessOutcome::pending;
        }
        for (const OutcomeSlot &slot : completed_) {
            if (slot.generation == generation) {
                return slot.outcome;
            }
        }
        if (cancelled_.generation == generation) {
            return cancelled_.outcome;
        }
        if (superseded_.generation == generation) {
            return superseded_.outcome;
        }
        return BrightnessOutcome::unknown;
    }

    [[nodiscard]] bool work_pending() const { return pending_.valid(); }
    [[nodiscard]] bool work_active() const { return active_.valid(); }

private:
    struct OutcomeSlot {
        std::uint32_t generation = 0;
        BrightnessOutcome outcome = BrightnessOutcome::unknown;
    };

    // Only two requests can leave the coordinator between caller polls: the
    // active request and its one waiting successor. Keep four completions so
    // a long burst of replaced previews cannot hide the older active result.
    static constexpr std::size_t kCompletedSlots = 4;
    std::array<OutcomeSlot, kCompletedSlots> completed_{};
    OutcomeSlot cancelled_{};
    OutcomeSlot superseded_{};
    BrightnessRequest pending_{};
    BrightnessRequest active_{};
    std::uint32_t generation_ = 0;
    std::size_t completed_index_ = 0;
};

}  // namespace runtime
}  // namespace chatesp
