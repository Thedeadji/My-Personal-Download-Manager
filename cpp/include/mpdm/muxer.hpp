// ffmpeg wrapper — concatenate segments and remux to MP4. Port of mpdm/muxer.py.
#pragma once
#include <optional>
#include <string>
#include <vector>

namespace mpdm {

// Binary-concatenate init + segments, then `ffmpeg -c copy` to out_path.
// Throws std::runtime_error if ffmpeg isn't found or fails. Returns out_path.
std::string assemble_segments(const std::vector<std::string>& seg_paths,
                              const std::string& out_path,
                              const std::optional<std::string>& init_path = std::nullopt,
                              bool cleanup = true);

} // namespace mpdm
