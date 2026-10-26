// HTTP helpers (probe + ranged GET) — port of mpdm/http_client.py, using libcurl.
#pragma once
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

#include "mpdm/models.hpp"

namespace mpdm {

struct HttpError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// The subset of Settings the HTTP layer needs (full config ported separately).
struct HttpOptions {
    std::string user_agent = "MyPersonalDownloadManager/0.1 (+research)";
    std::string proxy;                             // empty = direct
    long connect_timeout = 15;                     // seconds
    long low_speed_time = 60;                      // seconds under low_speed_limit -> abort
    long low_speed_limit = 1;                      // bytes/sec
    bool verify_tls = true;
    std::map<std::string, std::string> headers;    // extra (Cookie, Referer, custom UA)
};

// Filename helpers (ported).
std::string sanitize_filename(const std::string& name);
std::optional<std::string> filename_from_disposition(const std::string& content_disposition);
std::string filename_from_url(const std::string& url);

// Discover size, range support and filename without downloading the body (ranged
// GET of byte 0). Throws HttpError on a transport/HTTP failure.
RemoteInfo probe(const std::string& url, const HttpOptions& opt = {});

// Stream bytes [start, end] (end inclusive; end < 0 means open-ended) to `sink`.
// `sink(data, len)` returns false to abort. Returns the HTTP status code.
// Throws HttpError on a transport failure.
long get_range(const std::string& url, long long start, long long end,
               const HttpOptions& opt,
               const std::function<bool(const char*, size_t)>& sink);

} // namespace mpdm
