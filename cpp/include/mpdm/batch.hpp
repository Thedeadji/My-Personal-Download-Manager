// Batch / wildcard URL expansion — port of mpdm/batch.py.
#pragma once
#include <string>
#include <vector>

namespace mpdm {

constexpr size_t kMaxExpansion = 10000;  // safety cap per pattern

// Expand [1-100] / [001-010] / [3-1] / [a-e] range tokens in one URL (multiple
// ranges combine as a cartesian product). Throws std::length_error past the cap.
std::vector<std::string> expand_pattern(const std::string& url);

// Split text (newlines/spaces/commas) into URLs, expand each, dedupe (order kept).
std::vector<std::string> expand_urls(const std::string& text);

} // namespace mpdm
