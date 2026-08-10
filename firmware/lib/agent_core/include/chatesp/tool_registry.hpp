#pragma once

#include <array>
#include <cstddef>

#include "chatesp/agent_interfaces.hpp"

namespace chatesp {
namespace agent {

class Tool {
public:
    virtual ~Tool() = default;
    [[nodiscard]] virtual const char *name() const = 0;
    [[nodiscard]] virtual const char *description() const = 0;
    [[nodiscard]] virtual const char *parameters_schema() const = 0;
    [[nodiscard]] virtual bool ends_tool_sequence() const { return false; }
    virtual Error execute(
        const char *arguments, std::size_t size,
        FixedText<Limits::max_tool_result_bytes> &result,
        CancellationToken &cancellation) = 0;
};

class ToolRegistry {
public:
    Error add(Tool &tool);
    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] const Tool &at(std::size_t index) const {
        return *tools_[index];
    }
    [[nodiscard]] Tool *find(const char *name) const;
    [[nodiscard]] bool ends_tool_sequence(const ToolInvocation &call) const;
    Error execute(
        const ToolInvocation &call,
        FixedText<Limits::max_tool_result_bytes> &result,
        CancellationToken &cancellation) const;

private:
    std::array<Tool *, Limits::max_tool_count> tools_{};
    std::size_t size_ = 0;
};

class SearchWebTool final : public Tool {
public:
    explicit SearchWebTool(WebSearchProvider &provider) : provider_(provider) {}

    [[nodiscard]] const char *name() const override;
    [[nodiscard]] const char *description() const override;
    [[nodiscard]] const char *parameters_schema() const override;
    [[nodiscard]] bool ends_tool_sequence() const override { return true; }
    Error execute(
        const char *arguments, std::size_t size,
        FixedText<Limits::max_tool_result_bytes> &result,
        CancellationToken &cancellation) override;

private:
    WebSearchProvider &provider_;
    WebResults results_{};
    FixedText<Limits::max_tool_result_bytes> item_scratch_;
};

class SearchImagesTool final : public Tool {
public:
    explicit SearchImagesTool(ImageSearchProvider &provider)
        : provider_(provider) {}

    [[nodiscard]] const char *name() const override;
    [[nodiscard]] const char *description() const override;
    [[nodiscard]] const char *parameters_schema() const override;
    Error execute(
        const char *arguments, std::size_t size,
        FixedText<Limits::max_tool_result_bytes> &result,
        CancellationToken &cancellation) override;
    [[nodiscard]] const ImageResults &last_results() const {
        return last_results_;
    }
    [[nodiscard]] bool take_selected(ImageResult &result);
    [[nodiscard]] bool take_selected_or_first(ImageResult &result);
    [[nodiscard]] bool take_selected_or_first_candidates(
        ImageResults &results);
    [[nodiscard]] bool has_pending_image() const;
    void clear_results();

private:
    ImageSearchProvider &provider_;
    ImageResults last_results_{};
    ImageResult selected_result_{};
    FixedText<Limits::max_tool_result_bytes> item_scratch_;
    bool selection_ready_ = false;
    bool fallback_ready_ = false;
};

class RunPythonTool final : public Tool {
public:
    explicit RunPythonTool(PythonExecutionProvider &provider)
        : provider_(provider) {}

    [[nodiscard]] const char *name() const override;
    [[nodiscard]] const char *description() const override;
    [[nodiscard]] const char *parameters_schema() const override;
    [[nodiscard]] bool ends_tool_sequence() const override { return true; }
    Error execute(
        const char *arguments, std::size_t size,
        FixedText<Limits::max_tool_result_bytes> &result,
        CancellationToken &cancellation) override;
    [[nodiscard]] bool take_plot(PlotData &plot);
    void clear_plot();

private:
    PythonExecutionProvider &provider_;
    PlotData pending_plot_;
};

class PlotLineTool final : public Tool {
public:
    [[nodiscard]] const char *name() const override;
    [[nodiscard]] const char *description() const override;
    [[nodiscard]] const char *parameters_schema() const override;
    [[nodiscard]] bool ends_tool_sequence() const override { return true; }
    Error execute(
        const char *arguments, std::size_t size,
        FixedText<Limits::max_tool_result_bytes> &result,
        CancellationToken &cancellation) override;
    [[nodiscard]] bool take_plot(PlotData &plot);
    void clear_plot();

private:
    PlotData pending_plot_;
};

class GetDeviceStatusTool final : public Tool {
public:
    explicit GetDeviceStatusTool(DeviceControlProvider &provider)
        : provider_(provider) {}

    [[nodiscard]] const char *name() const override;
    [[nodiscard]] const char *description() const override;
    [[nodiscard]] const char *parameters_schema() const override;
    Error execute(
        const char *arguments, std::size_t size,
        FixedText<Limits::max_tool_result_bytes> &result,
        CancellationToken &cancellation) override;

private:
    DeviceControlProvider &provider_;
};

class SetBrightnessTool final : public Tool {
public:
    explicit SetBrightnessTool(DeviceControlProvider &provider)
        : provider_(provider) {}

    [[nodiscard]] const char *name() const override;
    [[nodiscard]] const char *description() const override;
    [[nodiscard]] const char *parameters_schema() const override;
    Error execute(
        const char *arguments, std::size_t size,
        FixedText<Limits::max_tool_result_bytes> &result,
        CancellationToken &cancellation) override;

private:
    DeviceControlProvider &provider_;
};

class SetVolumeTool final : public Tool {
public:
    explicit SetVolumeTool(DeviceControlProvider &provider)
        : provider_(provider) {}

    [[nodiscard]] const char *name() const override;
    [[nodiscard]] const char *description() const override;
    [[nodiscard]] const char *parameters_schema() const override;
    Error execute(
        const char *arguments, std::size_t size,
        FixedText<Limits::max_tool_result_bytes> &result,
        CancellationToken &cancellation) override;

private:
    DeviceControlProvider &provider_;
};

class PowerOffTool final : public Tool {
public:
    explicit PowerOffTool(DeviceControlProvider &provider)
        : provider_(provider) {}

    [[nodiscard]] const char *name() const override;
    [[nodiscard]] const char *description() const override;
    [[nodiscard]] const char *parameters_schema() const override;
    Error execute(
        const char *arguments, std::size_t size,
        FixedText<Limits::max_tool_result_bytes> &result,
        CancellationToken &cancellation) override;

private:
    DeviceControlProvider &provider_;
};

class RestartDeviceTool final : public Tool {
public:
    explicit RestartDeviceTool(DeviceControlProvider &provider)
        : provider_(provider) {}

    [[nodiscard]] const char *name() const override;
    [[nodiscard]] const char *description() const override;
    [[nodiscard]] const char *parameters_schema() const override;
    Error execute(
        const char *arguments, std::size_t size,
        FixedText<Limits::max_tool_result_bytes> &result,
        CancellationToken &cancellation) override;

private:
    DeviceControlProvider &provider_;
};

class RememberMemoryTool final : public Tool {
public:
    explicit RememberMemoryTool(MemoryControlProvider &provider)
        : provider_(provider) {}
    [[nodiscard]] const char *name() const override;
    [[nodiscard]] const char *description() const override;
    [[nodiscard]] const char *parameters_schema() const override;
    Error execute(
        const char *arguments, std::size_t size,
        FixedText<Limits::max_tool_result_bytes> &result,
        CancellationToken &cancellation) override;
private:
    MemoryControlProvider &provider_;
};

class ForgetMemoryTool final : public Tool {
public:
    explicit ForgetMemoryTool(MemoryControlProvider &provider)
        : provider_(provider) {}
    [[nodiscard]] const char *name() const override;
    [[nodiscard]] const char *description() const override;
    [[nodiscard]] const char *parameters_schema() const override;
    Error execute(
        const char *arguments, std::size_t size,
        FixedText<Limits::max_tool_result_bytes> &result,
        CancellationToken &cancellation) override;
private:
    MemoryControlProvider &provider_;
};

class ClearMemoriesTool final : public Tool {
public:
    explicit ClearMemoriesTool(MemoryControlProvider &provider)
        : provider_(provider) {}
    [[nodiscard]] const char *name() const override;
    [[nodiscard]] const char *description() const override;
    [[nodiscard]] const char *parameters_schema() const override;
    Error execute(
        const char *arguments, std::size_t size,
        FixedText<Limits::max_tool_result_bytes> &result,
        CancellationToken &cancellation) override;
private:
    MemoryControlProvider &provider_;
};

class CompactMemoriesTool final : public Tool {
public:
    explicit CompactMemoriesTool(MemoryControlProvider &provider)
        : provider_(provider) {}
    [[nodiscard]] const char *name() const override;
    [[nodiscard]] const char *description() const override;
    [[nodiscard]] const char *parameters_schema() const override;
    Error execute(
        const char *arguments, std::size_t size,
        FixedText<Limits::max_tool_result_bytes> &result,
        CancellationToken &cancellation) override;
private:
    MemoryControlProvider &provider_;
};

}  // namespace agent
}  // namespace chatesp
