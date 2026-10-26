// Resume sidecars (.mpdm) + destination helpers — port of mpdm/storage.py.
#pragma once
#include <optional>
#include <string>

#include "mpdm/models.hpp"

namespace mpdm {

std::string sidecar_path(const std::string& dest_path);
void save_record(const std::string& dest_path, const TaskRecord& rec);
std::optional<TaskRecord> load_record(const std::string& dest_path);
void clear_record(const std::string& dest_path);

// Reuse a matching interrupted download in `dir` (same url/size/etag) instead of
// creating a new (1),(2)… copy. Returns the dest path to resume, or none.
std::optional<std::string> find_resumable(const std::string& dir, const std::string& url,
                                          std::optional<long long> size,
                                          std::optional<std::string> etag);

// foo.zip -> foo (1).zip if the file or its sidecar already exist.
std::string unique_dest(const std::string& dir, const std::string& filename);

// Create a directory and mark it hidden (Windows). Returns the path.
std::string ensure_hidden_dir(const std::string& path);

} // namespace mpdm
