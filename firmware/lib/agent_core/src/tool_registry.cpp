#include "chatesp/tool_registry.hpp"

#include <cstdio>
#include <cstdint>
#include <cstring>

#include "json.hpp"

namespace chatesp {
namespace agent {
namespace {

constexpr char query_schema[] =
    "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\","
    "\"maxLength\":200}},\"required\":[\"query\"],"
    "\"additionalProperties\":false}";

constexpr char image_action_schema[] =
    "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\","
    "\"maxLength\":200},\"select\":{\"type\":\"string\",\"maxLength\":16}},"
    "\"oneOf\":[{\"required\":[\"query\"]},{\"required\":[\"select\"]}],"
    "\"additionalProperties\":false}";

constexpr char python_schema[] =
    "{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\","
    "\"maxLength\":1024}},\"required\":[\"code\"],"
    "\"additionalProperties\":false}";

constexpr char empty_object_schema[] =
    "{\"type\":\"object\",\"properties\":{},"
    "\"additionalProperties\":false}";

constexpr char brightness_schema[] =
    "{\"type\":\"object\",\"properties\":{\"percent\":{\"type\":"
    "\"integer\",\"minimum\":5,\"maximum\":100}},"
    "\"required\":[\"percent\"],\"additionalProperties\":false}";

constexpr char volume_schema[] =
    "{\"type\":\"object\",\"properties\":{\"percent\":{\"type\":"
    "\"integer\",\"minimum\":0,\"maximum\":100}},"
    "\"required\":[\"percent\"],\"additionalProperties\":false}";

enum class ImageActionKind : std::uint8_t { query, select };

struct ImageAction {
    ImageActionKind kind = ImageActionKind::query;
    FixedText<Limits::max_search_query_bytes> query;
    FixedText<Limits::max_image_id_bytes> selection;
};

template <typename T>
class ClearOnExit {
public:
    explicit ClearOnExit(T &value) : value_(value) {}
    ~ClearOnExit() { value_.clear(); }

    ClearOnExit(const ClearOnExit &) = delete;
    ClearOnExit &operator=(const ClearOnExit &) = delete;

private:
    T &value_;
};

Error parse_query(
    const char *arguments, std::size_t size,
    FixedText<Limits::max_search_query_bytes> &query) {
    if (arguments == nullptr || size == 0 ||
        size > Limits::max_tool_arguments_bytes) {
        return Error::invalid_argument;
    }
    detail::JsonReader reader(arguments, size);
    if (!reader.consume('{')) {
        return Error::malformed_response;
    }
    bool found = false;
    if (reader.peek() != '}') {
        while (true) {
            FixedText<32> key;
            if (!reader.read_string(key) || !reader.consume(':')) {
                return Error::malformed_response;
            }
            if (!key.equals("query") || found || !reader.read_string(query)) {
                return Error::invalid_argument;
            }
            found = true;
            if (reader.consume('}')) {
                break;
            }
            if (!reader.consume(',')) {
                return Error::malformed_response;
            }
        }
    } else {
        reader.consume('}');
    }
    if (!found || query.empty() || !reader.finish()) {
        return Error::invalid_argument;
    }
    return Error::none;
}

Error parse_python_source(
    const char *arguments, std::size_t size,
    FixedText<Limits::max_python_source_bytes> &source) {
    if (arguments == nullptr || size == 0 ||
        size > Limits::max_tool_arguments_bytes) {
        return Error::invalid_argument;
    }
    detail::JsonReader reader(arguments, size);
    if (!reader.consume('{')) {
        return Error::malformed_response;
    }
    FixedText<16> key;
    if (!reader.read_string(key) || !key.equals("code") ||
        !reader.consume(':') || !reader.read_string(source) ||
        !reader.consume('}') || !reader.finish() || source.empty()) {
        return Error::invalid_argument;
    }
    for (std::size_t index = 0; index < source.size(); ++index) {
        if (source.data()[index] == '\0') {
            source.clear();
            return Error::invalid_argument;
        }
    }
    return Error::none;
}

Error parse_image_action(
    const char *arguments, std::size_t size, ImageAction &action) {
    if (arguments == nullptr || size == 0 ||
        size > Limits::max_tool_arguments_bytes) {
        return Error::invalid_argument;
    }
    detail::JsonReader reader(arguments, size);
    if (!reader.consume('{')) {
        return Error::malformed_response;
    }
    bool found_query = false;
    bool found_selection = false;
    if (reader.peek() != '}') {
        while (true) {
            FixedText<32> key;
            if (!reader.read_string(key) || !reader.consume(':')) {
                return Error::malformed_response;
            }
            if (key.equals("query") && !found_query && !found_selection) {
                if (!reader.read_string(action.query)) {
                    return Error::invalid_argument;
                }
                found_query = true;
            } else if (
                key.equals("select") && !found_selection && !found_query) {
                if (!reader.read_string(action.selection)) {
                    return Error::invalid_argument;
                }
                found_selection = true;
            } else {
                return Error::invalid_argument;
            }
            if (reader.consume('}')) {
                break;
            }
            if (!reader.consume(',')) {
                return Error::malformed_response;
            }
        }
    } else {
        reader.consume('}');
    }
    if (!reader.finish() || found_query == found_selection) {
        return Error::invalid_argument;
    }
    if (found_query) {
        if (action.query.empty()) {
            return Error::invalid_argument;
        }
        action.kind = ImageActionKind::query;
        return Error::none;
    }
    if (action.selection.empty()) {
        return Error::invalid_argument;
    }
    action.kind = ImageActionKind::select;
    return Error::none;
}

Error parse_empty_object(const char *arguments, std::size_t size) {
    if (arguments == nullptr || size == 0 ||
        size > Limits::max_tool_arguments_bytes) {
        return Error::invalid_argument;
    }
    detail::JsonReader reader(arguments, size);
    return reader.consume('{') && reader.consume('}') && reader.finish()
        ? Error::none
        : Error::invalid_argument;
}

Error parse_percent(
    const char *arguments, std::size_t size, std::uint8_t minimum,
    std::uint8_t &percent) {
    if (arguments == nullptr || size == 0 ||
        size > Limits::max_tool_arguments_bytes) {
        return Error::invalid_argument;
    }
    detail::JsonReader reader(arguments, size);
    if (!reader.consume('{')) {
        return Error::malformed_response;
    }
    FixedText<16> key;
    std::uint32_t value = 0;
    if (!reader.read_string(key) || !key.equals("percent") ||
        !reader.consume(':') || !reader.read_u32(value) ||
        !reader.consume('}') || !reader.finish() || value < minimum ||
        value > 100) {
        return Error::invalid_argument;
    }
    percent = static_cast<std::uint8_t>(value);
    return Error::none;
}

const char *power_off_mode_name(PowerOffMode mode) {
    return mode == PowerOffMode::system_off
        ? "system_off"
        : "development_sleep";
}

bool append_boolean(
    FixedText<Limits::max_tool_result_bytes> &output, bool value) {
    return output.append(value ? "true" : "false");
}

template <std::size_t Capacity>
bool append_key(FixedText<Capacity> &output, const char *key) {
    return detail::append_json_string(output, key) && output.push_back(':');
}

bool append_web_result(
    FixedText<Limits::max_tool_result_bytes> &output, const WebResult &item) {
    return output.push_back('{') && append_key(output, "title") &&
           detail::append_json_string(output, item.title.data(), item.title.size()) &&
           output.push_back(',') && append_key(output, "url") &&
           detail::append_json_string(output, item.url.data(), item.url.size()) &&
           output.push_back(',') && append_key(output, "snippet") &&
           detail::append_json_string(
               output, item.snippet.data(), item.snippet.size()) &&
           output.push_back('}');
}

bool append_image_result(
    FixedText<Limits::max_tool_result_bytes> &output, const ImageResult &item) {
    return output.push_back('{') && append_key(output, "id") &&
           detail::append_json_string(output, item.id.data(), item.id.size()) &&
           output.push_back(',') && append_key(output, "title") &&
           detail::append_json_string(output, item.title.data(), item.title.size()) &&
           output.push_back(',') && append_key(output, "page_url") &&
           detail::append_json_string(
               output, item.page_url.data(), item.page_url.size()) &&
           output.push_back(',') && append_key(output, "thumbnail_url") &&
           detail::append_json_string(
               output, item.thumbnail_url.data(), item.thumbnail_url.size()) &&
           output.push_back('}');
}

}  // namespace

Error ToolRegistry::add(Tool &tool) {
    const char *tool_name = tool.name();
    const char *tool_description = tool.description();
    const char *schema = tool.parameters_schema();
    if (tool_name == nullptr || tool_description == nullptr || schema == nullptr ||
        std::strlen(tool_name) == 0 ||
        std::strlen(tool_name) > Limits::max_tool_name_bytes ||
        std::strlen(tool_description) > Limits::max_tool_description_bytes ||
        std::strlen(schema) > Limits::max_tool_schema_bytes ||
        !detail::valid_json_value(schema, std::strlen(schema))) {
        return Error::invalid_argument;
    }
    if (find(tool_name) != nullptr) {
        return Error::invalid_argument;
    }
    if (size_ == Limits::max_tool_count) {
        return Error::limit_exceeded;
    }
    tools_[size_++] = &tool;
    return Error::none;
}

Tool *ToolRegistry::find(const char *name) const {
    if (name == nullptr) {
        return nullptr;
    }
    for (std::size_t index = 0; index < size_; ++index) {
        if (std::strcmp(tools_[index]->name(), name) == 0) {
            return tools_[index];
        }
    }
    return nullptr;
}

Error ToolRegistry::execute(
    const ToolInvocation &call,
    FixedText<Limits::max_tool_result_bytes> &result,
    CancellationToken &cancellation) const {
    result.clear();
    if (cancellation.cancelled()) {
        return Error::cancelled;
    }
    Tool *tool = find(call.name.c_str());
    if (tool == nullptr) {
        return Error::tool_not_found;
    }
    const Error error = tool->execute(
        call.arguments.data(), call.arguments.size(), result, cancellation);
    if (error != Error::none) {
        result.clear();
    }
    return cancellation.cancelled() ? Error::cancelled : error;
}

const char *SearchWebTool::name() const { return "search_web"; }
const char *SearchWebTool::description() const {
    return "Search the current web for facts that can change.";
}
const char *SearchWebTool::parameters_schema() const { return query_schema; }

Error SearchWebTool::execute(
    const char *arguments, std::size_t size,
    FixedText<Limits::max_tool_result_bytes> &result,
    CancellationToken &cancellation) {
    FixedText<Limits::max_search_query_bytes> query;
    Error error = parse_query(arguments, size, query);
    if (error != Error::none) {
        return error;
    }
    results_.clear();
    item_scratch_.clear();
    ClearOnExit<WebResults> results_guard(results_);
    ClearOnExit<FixedText<Limits::max_tool_result_bytes>> item_guard(
        item_scratch_);
    error = provider_.search(
        query.data(), query.size(), results_, cancellation);
    if (error != Error::none) {
        return error;
    }
    if (!result.append("{\"results\":[")) {
        return Error::limit_exceeded;
    }
    for (std::size_t index = 0; index < results_.size; ++index) {
        item_scratch_.clear();
        if (!append_web_result(item_scratch_, results_.items[index])) {
            return Error::limit_exceeded;
        }
        const std::size_t separator = index == 0 ? 0 : 1;
        if (item_scratch_.size() + separator + 2 > result.remaining()) {
            break;
        }
        if (separator != 0 && !result.push_back(',')) {
            return Error::limit_exceeded;
        }
        if (!result.append(item_scratch_.data(), item_scratch_.size())) {
            return Error::limit_exceeded;
        }
    }
    return result.append("]}") ? Error::none : Error::limit_exceeded;
}

const char *SearchImagesTool::name() const { return "search_images"; }
const char *SearchImagesTool::description() const {
    return "Search for images, then select one current result by its ID.";
}
const char *SearchImagesTool::parameters_schema() const {
    return image_action_schema;
}

const char *RunPythonTool::name() const { return "run_python"; }

const char *RunPythonTool::description() const {
    return "Run bounded MicroPython math. Print needed values. For a line "
           "plot, call plot.line(x_list, y_list, optional_title).";
}

const char *RunPythonTool::parameters_schema() const { return python_schema; }

namespace {

const char *python_status_name(PythonExecutionStatus status) {
    switch (status) {
        case PythonExecutionStatus::ok:
            return "ok";
        case PythonExecutionStatus::script_error:
            return "script_error";
        case PythonExecutionStatus::memory_limit:
            return "memory_limit";
        case PythonExecutionStatus::output_limit:
            return "output_limit";
        case PythonExecutionStatus::time_limit:
            return "time_limit";
    }
    return "script_error";
}

bool append_bounded_python_output(
    FixedText<Limits::max_tool_result_bytes> &result, const char *text,
    std::size_t size, bool &truncated) {
    constexpr char suffix[] =
        ",\"output_truncated\":false,\"plot_ready\":false}";
    constexpr char hex[] = "0123456789abcdef";
    truncated = false;
    if (text == nullptr || !result.push_back('"')) {
        return false;
    }
    for (std::size_t index = 0; index < size;) {
        const unsigned char value = static_cast<unsigned char>(text[index]);
        char escaped[6]{};
        const char *token = text + index;
        std::size_t token_size = 1;
        std::size_t consumed = 1;
        if (value == '"' || value == '\\') {
            escaped[0] = '\\';
            escaped[1] = static_cast<char>(value);
            token = escaped;
            token_size = 2;
        } else if (value == '\b' || value == '\f' || value == '\n' ||
                   value == '\r' || value == '\t') {
            escaped[0] = '\\';
            escaped[1] = value == '\b' ? 'b' : value == '\f' ? 'f' :
                value == '\n' ? 'n' : value == '\r' ? 'r' : 't';
            token = escaped;
            token_size = 2;
        } else if (value < 0x20) {
            escaped[0] = '\\';
            escaped[1] = 'u';
            escaped[2] = '0';
            escaped[3] = '0';
            escaped[4] = hex[value >> 4];
            escaped[5] = hex[value & 0x0f];
            token = escaped;
            token_size = sizeof(escaped);
        } else if (value >= 0x80) {
            if (value >= 0xc2 && value <= 0xdf) {
                consumed = 2;
            } else if (value >= 0xe0 && value <= 0xef) {
                consumed = 3;
            } else if (value >= 0xf0 && value <= 0xf4) {
                consumed = 4;
            }
            bool valid = consumed != 1 && consumed <= size - index;
            for (std::size_t offset = 1; valid && offset < consumed; ++offset) {
                const unsigned char continuation =
                    static_cast<unsigned char>(text[index + offset]);
                valid = continuation >= 0x80 && continuation <= 0xbf;
            }
            if (valid && consumed == 3) {
                const unsigned char second =
                    static_cast<unsigned char>(text[index + 1]);
                valid = (value != 0xe0 || second >= 0xa0) &&
                    (value != 0xed || second <= 0x9f);
            } else if (valid && consumed == 4) {
                const unsigned char second =
                    static_cast<unsigned char>(text[index + 1]);
                valid = (value != 0xf0 || second >= 0x90) &&
                    (value != 0xf4 || second <= 0x8f);
            }
            if (valid) {
                token_size = consumed;
            } else {
                token = "?";
                token_size = 1;
                consumed = 1;
                truncated = true;
            }
        }
        if (token_size + 1 + sizeof(suffix) - 1 > result.remaining()) {
            truncated = true;
            break;
        }
        if (!result.append(token, token_size)) {
            return false;
        }
        index += consumed;
    }
    return result.push_back('"');
}

}  // namespace

Error RunPythonTool::execute(
    const char *arguments, std::size_t size,
    FixedText<Limits::max_tool_result_bytes> &result,
    CancellationToken &cancellation) {
    pending_plot_.clear();
    FixedText<Limits::max_python_source_bytes> source;
    Error error = parse_python_source(arguments, size, source);
    if (error != Error::none) {
        return error;
    }
    if (cancellation.cancelled()) {
        return Error::cancelled;
    }
    PythonExecution execution;
    error = provider_.execute(
        source.data(), source.size(), execution, cancellation);
    source.clear();
    if (error != Error::none || cancellation.cancelled()) {
        execution.clear();
        return cancellation.cancelled() ? Error::cancelled : error;
    }
    if (execution.status == PythonExecutionStatus::ok &&
        execution.plot.ready()) {
        pending_plot_ = execution.plot;
    }
    bool output_truncated = false;
    const bool built = result.append("{\"status\":") &&
        detail::append_json_string(result, python_status_name(execution.status)) &&
        result.append(",\"output\":") &&
        append_bounded_python_output(
            result, execution.output.data(), execution.output.size(),
            output_truncated) &&
        result.append(",\"output_truncated\":") &&
        append_boolean(result, output_truncated) &&
        result.append(",\"plot_ready\":") &&
        append_boolean(result, pending_plot_.ready()) && result.push_back('}');
    execution.clear();
    return built ? Error::none : Error::limit_exceeded;
}

bool RunPythonTool::take_plot(PlotData &plot) {
    if (!pending_plot_.ready()) {
        plot.clear();
        return false;
    }
    plot = pending_plot_;
    pending_plot_.clear();
    return true;
}

void RunPythonTool::clear_plot() { pending_plot_.clear(); }

Error SearchImagesTool::execute(
    const char *arguments, std::size_t size,
    FixedText<Limits::max_tool_result_bytes> &result,
    CancellationToken &cancellation) {
    ImageAction action;
    Error error = parse_image_action(arguments, size, action);
    if (error != Error::none) {
        return error;
    }
    if (cancellation.cancelled()) {
        return Error::cancelled;
    }
    if (action.kind == ImageActionKind::select) {
        for (std::size_t index = 0; index < last_results_.size; ++index) {
            const ImageResult &candidate = last_results_.items[index];
            if (candidate.id.size() == action.selection.size() &&
                std::memcmp(
                    candidate.id.data(), action.selection.data(),
                    action.selection.size()) == 0) {
                if (!result.append("{\"selected\":") ||
                    !detail::append_json_string(
                        result, candidate.id.data(), candidate.id.size()) ||
                    !result.push_back('}')) {
                    return Error::limit_exceeded;
                }
                selected_result_ = candidate;
                selection_ready_ = true;
                return Error::none;
            }
        }
        return Error::invalid_argument;
    }

    item_scratch_.clear();
    ClearOnExit<FixedText<Limits::max_tool_result_bytes>> item_guard(
        item_scratch_);
    selected_result_.clear();
    selection_ready_ = false;
    fallback_ready_ = false;
    last_results_.clear();
    error = provider_.search(
        action.query.data(), action.query.size(), last_results_, cancellation);
    if (error != Error::none) {
        return error;
    }
    fallback_ready_ = last_results_.size != 0;
    if (!result.append("{\"results\":[")) {
        return Error::limit_exceeded;
    }
    std::size_t written = 0;
    for (std::size_t index = 0; index < last_results_.size; ++index) {
        item_scratch_.clear();
        if (!append_image_result(item_scratch_, last_results_.items[index])) {
            return Error::limit_exceeded;
        }
        const std::size_t separator = written == 0 ? 0 : 1;
        if (item_scratch_.size() + separator + 2 > result.remaining()) {
            break;
        }
        if (separator != 0 && !result.push_back(',')) {
            return Error::limit_exceeded;
        }
        if (!result.append(item_scratch_.data(), item_scratch_.size())) {
            return Error::limit_exceeded;
        }
        ++written;
    }
    return result.append("]}") ? Error::none : Error::limit_exceeded;
}

bool SearchImagesTool::take_selected(ImageResult &result) {
    if (!selection_ready_) {
        result.clear();
        return false;
    }
    result = selected_result_;
    selected_result_.clear();
    selection_ready_ = false;
    fallback_ready_ = false;
    return true;
}

bool SearchImagesTool::take_selected_or_first(ImageResult &result) {
    if (take_selected(result)) {
        return true;
    }
    if (!fallback_ready_ || last_results_.size == 0) {
        result.clear();
        return false;
    }
    result = last_results_.items[0];
    fallback_ready_ = false;
    return true;
}

bool SearchImagesTool::has_pending_image() const {
    return selection_ready_ ||
        (fallback_ready_ && last_results_.size != 0);
}

void SearchImagesTool::clear_results() {
    last_results_.clear();
    selected_result_.clear();
    selection_ready_ = false;
    fallback_ready_ = false;
}

const char *GetDeviceStatusTool::name() const {
    return "get_device_status";
}

const char *GetDeviceStatusTool::description() const {
    return "Read brightness, volume, battery, persistence, and power-off mode.";
}

const char *GetDeviceStatusTool::parameters_schema() const {
    return empty_object_schema;
}

Error GetDeviceStatusTool::execute(
    const char *arguments, std::size_t size,
    FixedText<Limits::max_tool_result_bytes> &result,
    CancellationToken &cancellation) {
    const Error parse_error = parse_empty_object(arguments, size);
    if (parse_error != Error::none) {
        return parse_error;
    }
    if (cancellation.cancelled()) {
        return Error::cancelled;
    }
    DeviceStatus status;
    const Error error = provider_.status(status);
    if (error != Error::none) {
        return error;
    }
    char percentages[96]{};
    const int written = std::snprintf(
        percentages, sizeof(percentages),
        "{\"brightness_percent\":%u,\"volume_percent\":%u,"
        "\"battery_percent\":",
        static_cast<unsigned>(status.brightness_percent),
        static_cast<unsigned>(status.volume_percent));
    if (written <= 0 || static_cast<std::size_t>(written) >=
            sizeof(percentages) || !result.append(percentages)) {
        return Error::limit_exceeded;
    }
    if (status.battery_available) {
        char battery[4]{};
        const int battery_written = std::snprintf(
            battery, sizeof(battery), "%u",
            static_cast<unsigned>(status.battery_percent));
        if (battery_written <= 0 ||
            static_cast<std::size_t>(battery_written) >= sizeof(battery) ||
            !result.append(battery)) {
            return Error::limit_exceeded;
        }
    } else if (!result.append("null")) {
        return Error::limit_exceeded;
    }
    return result.append(",\"settings_persistent\":") &&
            append_boolean(result, status.settings_persistent) &&
            result.append(",\"power_off_mode\":") &&
            detail::append_json_string(
                result, power_off_mode_name(status.power_off_mode)) &&
            result.push_back('}')
        ? Error::none
        : Error::limit_exceeded;
}

const char *SetBrightnessTool::name() const { return "set_brightness"; }

const char *SetBrightnessTool::description() const {
    return "Set and save display brightness from 5 through 100 percent.";
}

const char *SetBrightnessTool::parameters_schema() const {
    return brightness_schema;
}

Error SetBrightnessTool::execute(
    const char *arguments, std::size_t size,
    FixedText<Limits::max_tool_result_bytes> &result,
    CancellationToken &cancellation) {
    std::uint8_t percent = 0;
    const Error parse_error = parse_percent(arguments, size, 5, percent);
    if (parse_error != Error::none) {
        return parse_error;
    }
    if (cancellation.cancelled()) {
        return Error::cancelled;
    }
    bool persisted = false;
    const Error error = provider_.set_brightness(percent, persisted);
    if (error != Error::none) {
        return error;
    }
    char prefix[40]{};
    const int written = std::snprintf(
        prefix, sizeof(prefix), "{\"brightness_percent\":%u,\"persisted\":",
        static_cast<unsigned>(percent));
    return written > 0 && static_cast<std::size_t>(written) < sizeof(prefix) &&
            result.append(prefix) && append_boolean(result, persisted) &&
            result.push_back('}')
        ? Error::none
        : Error::limit_exceeded;
}

const char *SetVolumeTool::name() const { return "set_volume"; }

const char *SetVolumeTool::description() const {
    return "Set and save spoken-answer volume from 0 through 100 percent.";
}

const char *SetVolumeTool::parameters_schema() const { return volume_schema; }

Error SetVolumeTool::execute(
    const char *arguments, std::size_t size,
    FixedText<Limits::max_tool_result_bytes> &result,
    CancellationToken &cancellation) {
    std::uint8_t percent = 0;
    const Error parse_error = parse_percent(arguments, size, 0, percent);
    if (parse_error != Error::none) {
        return parse_error;
    }
    if (cancellation.cancelled()) {
        return Error::cancelled;
    }
    bool persisted = false;
    const Error error = provider_.set_volume(percent, persisted);
    if (error != Error::none) {
        return error;
    }
    char prefix[36]{};
    const int written = std::snprintf(
        prefix, sizeof(prefix), "{\"volume_percent\":%u,\"persisted\":",
        static_cast<unsigned>(percent));
    return written > 0 && static_cast<std::size_t>(written) < sizeof(prefix) &&
            result.append(prefix) && append_boolean(result, persisted) &&
            result.push_back('}')
        ? Error::none
        : Error::limit_exceeded;
}

const char *PowerOffTool::name() const { return "power_off"; }

const char *PowerOffTool::description() const {
    return "Schedule power-off only when the user explicitly asks for it now.";
}

const char *PowerOffTool::parameters_schema() const {
    return empty_object_schema;
}

Error PowerOffTool::execute(
    const char *arguments, std::size_t size,
    FixedText<Limits::max_tool_result_bytes> &result,
    CancellationToken &cancellation) {
    const Error parse_error = parse_empty_object(arguments, size);
    if (parse_error != Error::none) {
        return parse_error;
    }
    if (cancellation.cancelled()) {
        return Error::cancelled;
    }
    PowerOffMode mode = PowerOffMode::system_off;
    const Error error = provider_.schedule_power_off(mode);
    if (error != Error::none) {
        return error;
    }
    return result.append("{\"scheduled\":true,\"mode\":") &&
            detail::append_json_string(result, power_off_mode_name(mode)) &&
            result.push_back('}')
        ? Error::none
        : Error::limit_exceeded;
}

}  // namespace agent
}  // namespace chatesp
