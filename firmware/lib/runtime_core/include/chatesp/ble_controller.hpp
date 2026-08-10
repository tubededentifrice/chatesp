#pragma once

#include <cstdint>

namespace chatesp {
namespace runtime {

enum class BleControllerTarget : std::uint8_t {
    stopped,
    running,
};

enum class BleControllerOperation : std::uint8_t {
    none,
    start,
    stop,
};

struct BleControllerWork {
    BleControllerOperation operation = BleControllerOperation::none;
    std::uint32_t generation = 0;

    [[nodiscard]] bool valid() const {
        return operation != BleControllerOperation::none;
    }
};

// This class has one owner. The caller can post a new target while an earlier
// operation runs. Completion then plans only the work that is needed for the
// latest target.
class BleControllerPlanner {
public:
    explicit BleControllerPlanner(
        bool actual_running = false,
        std::uint32_t initial_generation = 0);

    [[nodiscard]] std::uint32_t request(BleControllerTarget target);
    [[nodiscard]] BleControllerWork begin_next();
    [[nodiscard]] bool complete(
        BleControllerWork work, bool succeeded);

    [[nodiscard]] bool desired_running() const;
    [[nodiscard]] bool actual_running() const;
    [[nodiscard]] bool operation_active() const;
    [[nodiscard]] bool current_request_failed() const;
    [[nodiscard]] std::uint32_t generation() const;

private:
    bool desired_running_ = false;
    bool actual_running_ = false;
    bool current_request_failed_ = false;
    std::uint32_t generation_ = 0;
    BleControllerWork active_{};
};

}  // namespace runtime
}  // namespace chatesp
