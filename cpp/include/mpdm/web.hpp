// Local web dashboard — port of mpdm/web.py. A tiny JSON API over a
// DownloadManager plus one self-contained HTML page, served on localhost.
#pragma once
#include <string>

namespace mpdm {

// Serve the dashboard until interrupted. Returns a process exit code.
int run_web(const std::string& host = "127.0.0.1", int port = 8765, bool open_browser = true);

} // namespace mpdm
