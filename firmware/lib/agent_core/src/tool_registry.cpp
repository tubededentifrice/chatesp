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

}  // namespace agent
}  // namespace chatesp
