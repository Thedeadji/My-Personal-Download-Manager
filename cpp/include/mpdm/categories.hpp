// File-type categories + IDM-style folder routing — port of mpdm/categories.py.
#pragma once
#include <map>
#include <string>

namespace mpdm {

// Category name for a file ("Video"/"Audio"/"Documents"/"Archives"/"Programs"/"Other").
std::string classify(const std::string& filename);

// On-disk subfolder for a file's type ("Video"/"Music"/…), or "" for uncategorized.
std::string category_subdir(const std::string& filename);

// Resolve the save directory: base_dir, or base_dir/<Category> when enabled.
// `overrides` maps a category name to a custom path (absolute used as-is, relative
// joined to base_dir).
std::string categorized_dir(const std::string& base_dir, const std::string& filename,
                            bool enabled = true,
                            const std::map<std::string, std::string>& overrides = {});

} // namespace mpdm
