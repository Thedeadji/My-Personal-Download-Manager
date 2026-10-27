// High-level stream grabber: HLS/DASH manifest -> downloaded MP4.
// Port of mpdm/grabber.py — ties the parsers (hls, dash), the segment
// downloader and the muxer together.
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mpdm/config.hpp"
#include "mpdm/hls.hpp"

namespace mpdm {

// (segments_done, segments_total)
using GrabProgressCb = std::function<void(int, int)>;

// Per-segment AES-128-CBC (key, iv); nullopt = that segment is plaintext.
using DecryptEntry = std::optional<std::pair<std::string, std::string>>;

// Build a per-segment (key, iv) plan for an HLS media playlist. Returns an empty
// vector when nothing is encrypted. Throws std::runtime_error for methods other
// than AES-128 (e.g. SAMPLE-AES). Network-free except for key fetches (cached).
std::vector<DecryptEntry> hls_decrypt_plan(const MediaPlaylist& media, const HttpOptions& opt);

// Download an HLS or DASH stream to a single MP4 at out_path. Returns out_path.
// kind: "hls", "dash", or "auto" (guess from the URL). prefer: "best"/"worst".
// Requires ffmpeg on PATH (or $FFMPEG). Throws std::runtime_error on failure.
std::string grab(const std::string& url, const std::string& out_path,
                 const Settings& settings = {}, const std::string& kind = "auto",
                 const std::string& prefer = "best", const GrabProgressCb& progress_cb = {});

} // namespace mpdm
