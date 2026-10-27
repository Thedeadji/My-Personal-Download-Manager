#include "mpdm/hls.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <stdexcept>

namespace mpdm {

// ---- small helpers --------------------------------------------------------
static std::string strip(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static bool starts_with(const std::string& s, const char* p) {
    return s.rfind(p, 0) == 0;
}

// Parse comma-separated KEY=VALUE pairs, respecting quoted values. Keys upper-cased.
static std::map<std::string, std::string> parse_attributes(const std::string& line) {
    std::map<std::string, std::string> attrs;
    size_t i = 0, n = line.size();
    while (i < n) {
        size_t eq = line.find('=', i);
        if (eq == std::string::npos) break;
        std::string key = strip(line.substr(i, eq - i));
        for (auto& c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        size_t j = eq + 1;
        std::string val;
        if (j < n && line[j] == '"') {
            size_t end = line.find('"', j + 1);
            val = line.substr(j + 1, end - (j + 1));
            i = end + 2;
        } else {
            size_t comma = line.find(',', j);
            if (comma == std::string::npos) comma = n;
            val = strip(line.substr(j, comma - j));
            i = comma + 1;
        }
        attrs[key] = val;
    }
    return attrs;
}

static std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) out.push_back(line);
    return out;
}

std::optional<std::string> parse_iv(const std::string& raw0) {
    std::string raw = strip(raw0);
    if (raw.size() >= 2 && (raw[0] == '0') && (raw[1] == 'x' || raw[1] == 'X')) raw = raw.substr(2);
    if (raw.empty() || raw.size() % 2 != 0) return std::nullopt;
    std::string bytes;
    for (size_t i = 0; i + 1 < raw.size() + 1 && i + 1 < raw.size() + 1; i += 2) {
        if (i + 1 >= raw.size()) break;
        auto hexval = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = hexval(raw[i]), lo = hexval(raw[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        bytes.push_back(static_cast<char>((hi << 4) | lo));
    }
    if (bytes.empty()) return std::nullopt;
    // Right-justify to 16 bytes.
    if (bytes.size() < 16) bytes = std::string(16 - bytes.size(), '\0') + bytes;
    else if (bytes.size() > 16) bytes = bytes.substr(bytes.size() - 16);
    return bytes;
}

std::string segment_iv(const MediaSegment& seg) {
    if (seg.key && seg.key->iv) return *seg.key->iv;
    std::string iv(16, '\0');
    long long v = seg.seq;
    for (int i = 15; i >= 0; --i) { iv[i] = static_cast<char>(v & 0xFF); v >>= 8; }
    return iv;
}

bool MediaPlaylist::is_encrypted() const {
    for (const auto& s : seg_infos)
        if (s.key && s.key->method != "NONE" && !s.key->method.empty()) return true;
    return false;
}

bool is_master(const std::string& text) {
    return text.find("#EXT-X-STREAM-INF") != std::string::npos;
}

std::vector<Variant> parse_master(const std::string& text, const std::string& base_url) {
    std::vector<Variant> variants;
    auto lines = split_lines(text);
    for (auto& l : lines) l = strip(l);
    for (size_t idx = 0; idx < lines.size(); ++idx) {
        if (!starts_with(lines[idx], "#EXT-X-STREAM-INF")) continue;
        auto colon = lines[idx].find(':');
        auto attrs = parse_attributes(colon == std::string::npos ? "" : lines[idx].substr(colon + 1));
        std::string uri;
        for (size_t k = idx + 1; k < lines.size(); ++k) {
            if (!lines[k].empty() && lines[k][0] != '#') { uri = lines[k]; break; }
        }
        Variant v;
        v.uri = urljoin(base_url, uri);
        v.bandwidth = attrs.count("BANDWIDTH") ? std::atoi(attrs["BANDWIDTH"].c_str()) : 0;
        v.resolution = attrs.count("RESOLUTION") ? attrs["RESOLUTION"] : "";
        v.codecs = attrs.count("CODECS") ? attrs["CODECS"] : "";
        variants.push_back(std::move(v));
    }
    std::stable_sort(variants.begin(), variants.end(),
                     [](const Variant& a, const Variant& b) { return a.bandwidth > b.bandwidth; });
    return variants;
}

MediaPlaylist parse_media(const std::string& text, const std::string& base_url) {
    MediaPlaylist pl;
    std::optional<SegmentKey> current_key;
    long long seq = 0;
    for (auto raw : split_lines(text)) {
        std::string ln = strip(raw);
        if (ln.empty()) continue;
        if (starts_with(ln, "#EXT-X-TARGETDURATION")) {
            auto c = ln.find(':');
            if (c != std::string::npos) pl.target_duration = std::atof(ln.substr(c + 1).c_str());
        } else if (starts_with(ln, "#EXT-X-MEDIA-SEQUENCE")) {
            auto c = ln.find(':');
            if (c != std::string::npos) { pl.media_sequence = std::atoll(ln.substr(c + 1).c_str()); seq = pl.media_sequence; }
        } else if (starts_with(ln, "#EXT-X-MAP")) {
            auto attrs = parse_attributes(ln.substr(ln.find(':') + 1));
            if (attrs.count("URI")) pl.init_segment = urljoin(base_url, attrs["URI"]);
        } else if (starts_with(ln, "#EXT-X-KEY")) {
            auto attrs = parse_attributes(ln.substr(ln.find(':') + 1));
            std::string method = attrs.count("METHOD") ? attrs["METHOD"] : "NONE";
            if (method == "NONE") {
                current_key.reset();
            } else {
                SegmentKey k;
                k.method = method;
                if (attrs.count("URI")) k.uri = urljoin(base_url, attrs["URI"]);
                if (attrs.count("IV")) k.iv = parse_iv(attrs["IV"]);
                current_key = k;
                if (pl.key_method == "NONE") { pl.key_method = method; pl.key_uri = k.uri; }
            }
        } else if (starts_with(ln, "#EXT-X-ENDLIST")) {
            pl.is_endlist = true;
        } else if (ln[0] != '#') {
            std::string seg_url = urljoin(base_url, ln);
            pl.segments.push_back(seg_url);
            MediaSegment ms;
            ms.uri = seg_url;
            ms.seq = seq;
            ms.key = current_key;
            pl.seg_infos.push_back(ms);
            ++seq;
        }
    }
    return pl;
}

std::string fetch_key(const std::string& uri, const HttpOptions& opt) {
    std::string key = get_bytes(uri, opt);
    if (key.size() != 16)
        throw std::runtime_error("AES-128 key at " + uri + " was " + std::to_string(key.size()) +
                                 " bytes, expected 16");
    return key;
}

std::pair<MediaPlaylist, std::string> resolve_media_playlist(const std::string& url,
                                                             const HttpOptions& opt,
                                                             const std::string& prefer) {
    std::string text = get_bytes(url, opt);
    if (is_master(text)) {
        auto variants = parse_master(text, url);
        if (variants.empty()) throw std::runtime_error("master playlist had no variants");
        Variant chosen = variants.front();
        if (prefer == "worst") {
            chosen = variants.back();
        } else if (prefer != "best" && !prefer.empty()) {
            for (const auto& v : variants)
                if (v.resolution.find(prefer) != std::string::npos) { chosen = v; break; }
        }
        std::string media_text = get_bytes(chosen.uri, opt);
        return {parse_media(media_text, chosen.uri), chosen.uri};
    }
    return {parse_media(text, url), url};
}

} // namespace mpdm
