// MPEG-DASH (.mpd) parsing — port of mpdm/dash.py (SegmentTemplate/Timeline,
// SegmentList, SegmentBase, bare BaseURL).
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "mpdm/http_client.hpp"

namespace mpdm {

struct DashStream {
    std::optional<std::string> init_url;
    std::vector<std::string> segment_urls;
    std::string mime;
    int bandwidth = 0;
};

// Parse already-fetched MPD XML. Throws std::runtime_error on an unsupported MPD.
DashStream parse_manifest(const std::string& text, const std::string& url,
                          const std::string& prefer = "best");

// Fetch then parse.
DashStream parse_mpd(const std::string& url, const HttpOptions& opt,
                     const std::string& prefer = "best");

} // namespace mpdm
