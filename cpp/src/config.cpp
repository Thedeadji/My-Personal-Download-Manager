#include "mpdm/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "mpdm/ratelimit.hpp"

namespace mpdm {

namespace fs = std::filesystem;

#ifndef _WIN32
// XDG_DOWNLOAD_DIR from ~/.config/user-dirs.dirs (localized, e.g. ~/Unduhan), or "".
static std::string xdg_download_dir(const std::string& home) {
    const char* cfg = std::getenv("XDG_CONFIG_HOME");
    fs::path f = (cfg && *cfg) ? fs::path(cfg) / "user-dirs.dirs" : fs::path(home) / ".config" / "user-dirs.dirs";
    std::ifstream in(f);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("XDG_DOWNLOAD_DIR=", 0) != 0) continue;
        std::string v = line.substr(17);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        if (v.rfind("$HOME", 0) == 0) v = home + v.substr(5);
        return v;
    }
    return "";
}
#endif

std::string default_download_dir() {
    const char* home = std::getenv("USERPROFILE");
    if (!home) home = std::getenv("HOME");
    if (!home || !*home) return ".";  // last resort; relative but better than crashing
    fs::path dl = fs::path(home) / "Downloads";
#ifndef _WIN32
    if (std::string x = xdg_download_dir(home); !x.empty()) dl = x;
#endif
    std::error_code ec;
    if (!fs::is_directory(dl, ec)) fs::create_directories(dl, ec);
    // Fall back to the home folder itself if Downloads can't be made.
    return fs::is_directory(dl, ec) ? dl.string() : std::string(home);
}

static std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string strip(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Read an env var; returns nullptr-safe optional-ish via out param.
static bool env(const char* name, std::string& out) {
    const char* v = std::getenv(name);
    if (!v) return false;
    out = v;
    return true;
}

std::string resolve_user_agent(const std::string& value) {
    std::string v = strip(value);
    if (v.empty() || lower(v) == "default" || lower(v) == "honest") return DEFAULT_USER_AGENT;
    if (lower(v) == "browser" || lower(v) == "chrome") return BROWSER_USER_AGENT;
    return v;
}

static bool truthy(const std::string& v) {
    std::string s = lower(strip(v));
    return !(s == "0" || s == "false" || s == "no" || s.empty());
}

void apply_env(Settings& s) {
    std::string v;
    if (env("MDM_CONNECTIONS", v)) {
        try {
            s.connections = std::max(1, std::stoi(v));
        } catch (...) {
        }
    }
    if (env("MDM_OUTPUT_DIR", v)) s.output_dir = v;
    if (env("MDM_MAX_RATE", v)) {
        try {
            s.max_rate_bps = parse_rate(v);
        } catch (...) {
        }
    }
    if (env("MDM_USER_AGENT", v)) s.user_agent = resolve_user_agent(v);
    if (env("MDM_CATEGORIZE", v)) s.categorize = truthy(v);
    if (env("MDM_PROXY", v)) s.proxy = strip(v);
    if (env("MDM_DYNAMIC", v)) s.dynamic_segmentation = truthy(v);
    if (env("MDM_DUPLICATE", v)) {
        std::string p = lower(strip(v));
        if (p == "rename" || p == "overwrite" || p == "skip" || p == "ask") s.duplicate_policy = p;
    }
}

Settings settings_from_env() {
    Settings s;
    apply_env(s);
    return s;
}

} // namespace mpdm
