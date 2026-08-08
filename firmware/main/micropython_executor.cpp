#include "micropython_executor.hpp"

#include <array>
#include <cstdint>

#include "chatesp_micropython.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

namespace chatesp {
namespace {

void secure_wipe(void *data, std::size_t size) {
    auto *bytes = static_cast<volatile std::uint8_t *>(data);
    while (size-- != 0) {
        *bytes++ = 0;
    }
}

bool execution_cancelled(void *context) {
    return context != nullptr &&
        static_cast<agent::CancellationToken *>(context)->cancelled();
}

std::int64_t monotonic_us(void *) { return esp_timer_get_time(); }

agent::PythonExecutionStatus execution_status(
    chatesp_micropython_result_t result) {
    switch (result) {
        case CHATESP_MICROPYTHON_OK:
            return agent::PythonExecutionStatus::ok;
        case CHATESP_MICROPYTHON_MEMORY_LIMIT:
            return agent::PythonExecutionStatus::memory_limit;
        case CHATESP_MICROPYTHON_OUTPUT_LIMIT:
            return agent::PythonExecutionStatus::output_limit;
        case CHATESP_MICROPYTHON_TIME_LIMIT:
            return agent::PythonExecutionStatus::time_limit;
        case CHATESP_MICROPYTHON_SCRIPT_ERROR:
        case CHATESP_MICROPYTHON_INVALID_ARGUMENT:
        case CHATESP_MICROPYTHON_BUSY:
        case CHATESP_MICROPYTHON_CANCELLED:
            return agent::PythonExecutionStatus::script_error;
    }
    return agent::PythonExecutionStatus::script_error;
}

}  // namespace

MicroPythonExecutor::MicroPythonExecutor() {
    heap_ = heap_caps_malloc(
        agent::Limits::python_heap_bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (heap_ != nullptr) {
        secure_wipe(heap_, agent::Limits::python_heap_bytes);
    }
}

MicroPythonExecutor::~MicroPythonExecutor() {
    if (heap_ != nullptr) {
        secure_wipe(heap_, agent::Limits::python_heap_bytes);
        heap_caps_free(heap_);
        heap_ = nullptr;
    }
}

agent::Error MicroPythonExecutor::execute(
    const char *source, std::size_t size,
    agent::PythonExecution &execution,
    agent::CancellationToken &cancellation) {
    execution.clear();
    if (source == nullptr || size == 0 ||
        size > agent::Limits::max_python_source_bytes) {
        return agent::Error::invalid_argument;
    }
    if (cancellation.cancelled()) {
        return agent::Error::cancelled;
    }
    if (heap_ == nullptr) {
        return agent::Error::model_failed;
    }

    std::array<char, agent::Limits::max_python_output_bytes + 1> output{};
    chatesp_micropython_plot_t plot{};
    chatesp_micropython_config_t config{
        .output = output.data(),
        .output_capacity = output.size(),
        .output_size = 0,
        .plot = &plot,
        .cancelled = execution_cancelled,
        .clock_us = monotonic_us,
        .callback_context = &cancellation,
        .maximum_duration_us =
            static_cast<std::int64_t>(
                agent::Limits::python_maximum_duration_ms) * 1'000,
        .maximum_vm_hooks = agent::Limits::python_maximum_vm_hooks,
    };
    const chatesp_micropython_result_t result =
        chatesp_micropython_execute(
            source, size, heap_, agent::Limits::python_heap_bytes,
            agent::Limits::python_stack_limit_bytes, &config);

    execution.status = execution_status(result);
    const std::size_t output_size =
        config.output_size <= agent::Limits::max_python_output_bytes
        ? config.output_size
        : agent::Limits::max_python_output_bytes;
    if (!execution.output.assign(output.data(), output_size)) {
        execution.clear();
        secure_wipe(output.data(), output.size());
        secure_wipe(&plot, sizeof(plot));
        secure_wipe(heap_, agent::Limits::python_heap_bytes);
        return agent::Error::limit_exceeded;
    }
    if (result == CHATESP_MICROPYTHON_OK && plot.ready &&
        plot.count >= 2 && plot.count <= agent::Limits::max_plot_points) {
        execution.plot.count = plot.count;
        for (std::size_t index = 0; index < plot.count; ++index) {
            execution.plot.x[index] = plot.x[index];
            execution.plot.y[index] = plot.y[index];
        }
        if (!execution.plot.title.assign(plot.title)) {
            execution.clear();
            secure_wipe(output.data(), output.size());
            secure_wipe(&plot, sizeof(plot));
            secure_wipe(heap_, agent::Limits::python_heap_bytes);
            return agent::Error::limit_exceeded;
        }
    }

    secure_wipe(output.data(), output.size());
    secure_wipe(&plot, sizeof(plot));
    secure_wipe(heap_, agent::Limits::python_heap_bytes);
    if (result == CHATESP_MICROPYTHON_CANCELLED ||
        cancellation.cancelled()) {
        execution.clear();
        return agent::Error::cancelled;
    }
    if (result == CHATESP_MICROPYTHON_INVALID_ARGUMENT ||
        result == CHATESP_MICROPYTHON_BUSY) {
        execution.clear();
        return agent::Error::model_failed;
    }
    return agent::Error::none;
}

}  // namespace chatesp
