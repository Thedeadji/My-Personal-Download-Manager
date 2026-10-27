// Resume sidecars (.mpdm) + destination helpers — port of mpdm/storage.py.
#pragma once
#include <optional>
#include <string>
#include <vector>

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

// Summary of what clean_dir removed/kept.
struct CleanResult {
    std::vector<std::string> sidecars;      // *.mpdm removed
    std::vector<std::string> partials;      // incomplete target files removed (purge only)
    std::vector<std::string> tmp;           // *.tmp checkpoints removed
    std::vector<std::string> kept_locked;   // couldn't remove (in use)
};

// Remove orphaned resume sidecars (and .tmp checkpoints) in `dir`. With
// purge_partials, also delete the incomplete target file each stale sidecar names.
CleanResult clean_dir(const std::string& dir, bool purge_partials = false);

} // namespace mpdm
