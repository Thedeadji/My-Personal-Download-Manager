// Google Drive download resolution — port of mpdm/gdrive.py.
// Rewrites a Drive share/download URL to the real file URL, following the
// confirm-token interstitial, and recovers the real filename.
#pragma once
#include <optional>
#include <string>

#include "mpdm/http_client.hpp"

namespace mpdm {

struct GDriveResult {
    std::string url;                       // the URL to actually download
    std::optional<std::string> filename;   // real name, if recovered
};

bool is_drive_url(const std::string& url);
std::optional<std::string> drive_file_id(const std::string& url);

// Resolve a Drive link to (download_url, filename?). Never throws: on any miss or
// error it returns the original URL and no filename.
GDriveResult resolve_drive(const std::string& url, const HttpOptions& opt = {});

} // namespace mpdm
