// Settings — the subset of mpdm/config.py the engine/manager need.
#pragma once
#include <map>
#include <string>

namespace mpdm {

inline constexpr const char* DEFAULT_USER_AGENT = "MyPersonalDownloadManager/0.1 (+research)";
inline constexpr const char* BROWSER_USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";

struct Settings {
    int connections = 8;
    long long min_segment_size = 256 * 1024;   // 256 KiB
    long connect_timeout = 15;                  // seconds
    long read_timeout = 60;                     // seconds (libcurl low-speed window)
    int max_retries = 5;
    double retry_backoff = 1.5;
    std::string user_agent = DEFAULT_USER_AGENT;
    std::string output_dir = ".";
    bool verify_tls = true;
    std::string proxy;
    long long max_rate_bps = 0;                 // 0 = unlimited (per-download)
    bool categorize = true;
    std::map<std::string, std::string> category_dirs;
    bool dynamic_segmentation = true;
};

} // namespace mpdm
