#include "mpdm/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

#include "mpdm/ratelimit.hpp"

namespace mpdm {

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
}

Settings settings_from_env() {
    Settings s;
    apply_env(s);
    return s;
}

} // namespace mpdm
