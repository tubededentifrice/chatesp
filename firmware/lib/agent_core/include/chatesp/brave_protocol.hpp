#pragma once

#include <cstddef>

#include "chatesp/agent_types.hpp"

namespace chatesp {
namespace agent {

struct BraveSearchOptions {
    const char *country = "ALL";
    const char *search_language = "en";
};

using SearchRequestTarget = FixedText<2'048>;

Error build_brave_web_search_target(
    const char *query, std::size_t size, const BraveSearchOptions &options,
    SearchRequestTarget &target);

Error build_brave_image_search_target(
    const char *query, std::size_t size, const BraveSearchOptions &options,
    SearchRequestTarget &target);

Error parse_brave_web_response(
    const char *json, std::size_t size, WebResults &results);

Error parse_brave_image_response(
    const char *json, std::size_t size, ImageResults &results);

}  // namespace agent
}  // namespace chatesp
