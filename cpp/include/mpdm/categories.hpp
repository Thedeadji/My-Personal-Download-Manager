// File-type categories + IDM-style folder routing — port of mpdm/categories.py.
#pragma once
#include <map>
#include <string>

namespace mpdm {

// Category name for a file ("Video"/"Audio"/"Documents"/"Archives"/"Programs"/"Other",
// or a user-defined category). `extra` maps a category name to space-separated
// extensions (no dots) that take priority over the built-in table.
std::string classify(const std::string& filename,
                     const std::map<std::string, std::string>& extra = {});

// On-disk subfolder for a file's type ("Video"/"Music"/…), or "" for uncategorized.
std::string category_subdir(const std::string& filename,
                            const std::map<std::string, std::string>& extra = {});

// The on-disk subfolder for a category name (built-in mapping; a custom category
// uses its own name; "Other" stays in the base dir).
std::string category_folder(const std::string& category);

// Resolve the save directory: base_dir, or base_dir/<Category> when enabled.
// `overrides` maps a category name to a custom path (absolute used as-is, relative
// joined to base_dir). `extra` adds user-defined extensions (see classify).
std::string categorized_dir(const std::string& base_dir, const std::string& filename,
                            bool enabled = true,
                            const std::map<std::string, std::string>& overrides = {},
                            const std::map<std::string, std::string>& extra = {});

} // namespace mpdm
