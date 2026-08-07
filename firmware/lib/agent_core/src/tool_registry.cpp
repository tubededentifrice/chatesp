#include "chatesp/tool_registry.hpp"

#include <cstdio>
#include <cstring>

#include "json.hpp"

namespace chatesp {
namespace agent {
namespace {

constexpr char query_schema[] =
    "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\","
    "\"maxLength\":200}},\"required\":[\"query\"],"
    "\"additionalProperties\":false}";

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
    WebResults results;
    error = provider_.search(query.data(), query.size(), results, cancellation);
    if (error != Error::none) {
        return error;
    }
    if (!result.append("{\"results\":[")) {
        return Error::limit_exceeded;
    }
    for (std::size_t index = 0; index < results.size; ++index) {
        FixedText<Limits::max_tool_result_bytes> item;
        if (!append_web_result(item, results.items[index])) {
            return Error::limit_exceeded;
        }
        const std::size_t separator = index == 0 ? 0 : 1;
        if (item.size() + separator + 2 > result.remaining()) {
            break;
        }
        if (separator != 0 && !result.push_back(',')) {
            return Error::limit_exceeded;
        }
        if (!result.append(item.data(), item.size())) {
            return Error::limit_exceeded;
        }
    }
    return result.append("]}") ? Error::none : Error::limit_exceeded;
}

const char *SearchImagesTool::name() const { return "search_images"; }
const char *SearchImagesTool::description() const {
    return "Search for an image when a visual result helps the user.";
}
const char *SearchImagesTool::parameters_schema() const { return query_schema; }

Error SearchImagesTool::execute(
    const char *arguments, std::size_t size,
    FixedText<Limits::max_tool_result_bytes> &result,
    CancellationToken &cancellation) {
    FixedText<Limits::max_search_query_bytes> query;
    Error error = parse_query(arguments, size, query);
    if (error != Error::none) {
        return error;
    }
    last_results_ = {};
    error = provider_.search(
        query.data(), query.size(), last_results_, cancellation);
    if (error != Error::none) {
        return error;
    }
    if (!result.append("{\"results\":[")) {
        return Error::limit_exceeded;
    }
    std::size_t written = 0;
    for (std::size_t index = 0; index < last_results_.size; ++index) {
        FixedText<Limits::max_tool_result_bytes> item;
        if (!append_image_result(item, last_results_.items[index])) {
            return Error::limit_exceeded;
        }
        const std::size_t separator = written == 0 ? 0 : 1;
        if (item.size() + separator + 2 > result.remaining()) {
            break;
        }
        if (separator != 0 && !result.push_back(',')) {
            return Error::limit_exceeded;
        }
        if (!result.append(item.data(), item.size())) {
            return Error::limit_exceeded;
        }
        ++written;
    }
    return result.append("]}") ? Error::none : Error::limit_exceeded;
}

}  // namespace agent
}  // namespace chatesp
