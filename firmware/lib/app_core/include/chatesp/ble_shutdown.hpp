#pragma once

#include <cstdint>

namespace chatesp {
namespace runtime {

class BleShutdown {
public:
    enum class Step : std::uint8_t {
        stop_host,
        deinitialize_host,
        complete,
    };

    [[nodiscard]] Step step() const { return step_; }

    void host_stopped() {
        if (step_ == Step::stop_host) {
            step_ = Step::deinitialize_host;
        }
    }

    void host_deinitialized() {
        if (step_ == Step::deinitialize_host) {
            step_ = Step::complete;
        }
    }

    void reset() { step_ = Step::stop_host; }

private:
    Step step_ = Step::stop_host;
};

}  // namespace runtime
}  // namespace chatesp
