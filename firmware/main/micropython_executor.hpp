#pragma once

#include "chatesp/agent_interfaces.hpp"

namespace chatesp {

class MicroPythonExecutor final : public agent::PythonExecutionProvider {
public:
    MicroPythonExecutor();
    ~MicroPythonExecutor() override;

    MicroPythonExecutor(const MicroPythonExecutor &) = delete;
    MicroPythonExecutor &operator=(const MicroPythonExecutor &) = delete;

    [[nodiscard]] bool available() const { return heap_ != nullptr; }

    agent::Error execute(
        const char *source, std::size_t size,
        agent::PythonExecution &execution,
        agent::CancellationToken &cancellation) override;

private:
    void *heap_ = nullptr;
};

}  // namespace chatesp
