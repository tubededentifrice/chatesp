#include "chatesp/ble_controller.hpp"

namespace chatesp {
namespace runtime {

BleControllerPlanner::BleControllerPlanner(
    bool actual_running, std::uint32_t initial_generation)
    : desired_running_(actual_running),
      actual_running_(actual_running),
      generation_(initial_generation) {}

std::uint32_t BleControllerPlanner::request(BleControllerTarget target) {
    const bool requested_running = target == BleControllerTarget::running;
    if (requested_running != desired_running_ || current_request_failed_) {
        desired_running_ = requested_running;
        ++generation_;
        current_request_failed_ = false;
    }
    return generation_;
}

BleControllerWork BleControllerPlanner::begin_next() {
    if (active_.valid() || current_request_failed_ ||
        desired_running_ == actual_running_) {
        return {};
    }
    active_ = {
        desired_running_ ? BleControllerOperation::start
                         : BleControllerOperation::stop,
        generation_,
    };
    return active_;
}

bool BleControllerPlanner::complete(
    BleControllerWork work, bool succeeded) {
    if (!active_.valid() || work.operation != active_.operation ||
        work.generation != active_.generation) {
        return false;
    }

    if (succeeded) {
        actual_running_ =
            work.operation == BleControllerOperation::start;
    }
    active_ = {};
    current_request_failed_ =
        !succeeded && work.generation == generation_ &&
        desired_running_ != actual_running_;
    return true;
}

bool BleControllerPlanner::desired_running() const {
    return desired_running_;
}

bool BleControllerPlanner::actual_running() const {
    return actual_running_;
}

bool BleControllerPlanner::operation_active() const {
    return active_.valid();
}

bool BleControllerPlanner::current_request_failed() const {
    return current_request_failed_;
}

std::uint32_t BleControllerPlanner::generation() const {
    return generation_;
}

}  // namespace runtime
}  // namespace chatesp
