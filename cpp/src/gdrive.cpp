#include "mpdm/gdrive.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <regex>
#include <string>

namespace mpdm {

static const char* kDownload = "https://drive.usercontent.google.com/download";

static std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// host portion of a URL (between scheme:// and the next / ? #).
static std::string url_host(const std::string& url) {
    auto p = url.find("://");
    size_t start = (p == std::string::npos) ? 0 : p + 3;
    size_t end = url.find_first_of("/?#", start);
    return lower(url.substr(start, end == std::string::npos ? std::string::npos : end - start));
}

static std::string url_query(const std::string& url) {
    auto q = url.find('?');
    if (q == std::string::npos) return "";
    std::string s = url.substr(q + 1);
    auto h = s.find('#');
    return h == std::string::npos ? s : s.substr(0, h);
}

// value of query param `key`, or empty.
static std::string query_param(const std::string& query, const std::string& key) {
    std::regex re("(?:^|&)" + key + "=([^&]*)");
    std::smatch m;
    if (std::regex_search(query, m, re)) return m[1].str();
    return "";
}

bool is_drive_url(const std::string& url) {
    std::string host = url_host(url);
    static const char* hosts[] = {"drive.google.com", "drive.usercontent.google.com"};
    for (const char* h : hosts)
        if (host == h || (host.size() > std::strlen(h) && host.rfind(std::string(".") + h) ==
                                                              host.size() - std::strlen(h) - 1))
            return true;
    return false;
}

std::optional<std::string> drive_file_id(const std::string& url) {
    std::smatch m;
    if (std::regex_search(url, m, std::regex("/file/d/([A-Za-z0-9_-]{10,})"))) return m[1].str();
    std::string id = query_param(url_query(url), "id");
    if (!id.empty()) return id;
    if (std::regex_search(url, m, std::regex("/d/([A-Za-z0-9_-]{10,})"))) return m[1].str();
    return std::nullopt;
}

// Minimal percent-encoding for query values (keep RFC3986 unreserved chars).
static std::string pct_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

static std::optional<std::string> name_from_html(const std::string& html) {
    std::smatch m;
    // The "download anyway" page shows the name in a uc-name-size span.
    if (std::regex_search(html, m,
                          std::regex("<span[^>]*class=\"uc-name-size\"[^>]*>[\\s\\S]*?>([^<]+)</a>",
                                     std::regex::icase))) {
        std::string s = m[1].str();
        return s;
    }
    if (std::regex_search(html, m, std::regex("<title>([^<]+)</title>", std::regex::icase))) {
        std::string t = m[1].str();
        for (const std::string& suf : {std::string(" - Google Drive"), std::string(" \xE2\x80\x93 Google Drive")}) {
            if (t.size() >= suf.size() && t.compare(t.size() - suf.size(), suf.size(), suf) == 0) {
                t = t.substr(0, t.size() - suf.size());
                break;
            }
        }
        // trim
        auto a = t.find_first_not_of(" \t\r\n");
        auto b = t.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) return std::nullopt;
        return t.substr(a, b - a + 1);
    }
    return std::nullopt;
}

static std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) {
        s.replace(p, from.size(), to);
        p += to.size();
    }
    return s;
}

// Build the real download URL from the interstitial's <form>.
static std::optional<std::string> parse_download_form(const std::string& html) {
    std::smatch m;
    std::string action;
    if (std::regex_search(html, m,
                          std::regex("id=\"download-form\"[^>]*action=\"([^\"]+)\"", std::regex::icase))) {
        action = m[1].str();
    } else if (std::regex_search(
                   html, m,
                   std::regex("<form[^>]+action=\"([^\"]*(?:download|uc)[^\"]*)\"", std::regex::icase))) {
        action = m[1].str();
    } else {
        return std::nullopt;
    }
    action = replace_all(action, "&amp;", "&");

    std::string query;
    std::regex in_re("<input[^>]+name=\"([^\"]+)\"[^>]+value=\"([^\"]*)\"", std::regex::icase);
    for (auto it = std::sregex_iterator(html.begin(), html.end(), in_re); it != std::sregex_iterator();
         ++it) {
        if (!query.empty()) query += "&";
        query += pct_encode((*it)[1].str()) + "=" + pct_encode((*it)[2].str());
    }
    if (query.empty()) return action;
    return action + (action.find('?') != std::string::npos ? "&" : "?") + query;
}

GDriveResult resolve_drive(const std::string& url, const HttpOptions& opt) {
    auto fid = drive_file_id(url);
    if (!fid) return {url, std::nullopt};

    bool tokenized = !query_param(url_query(url), "confirm").empty();
    std::string target = tokenized ? url
                                   : (std::string(kDownload) + "?id=" + pct_encode(*fid) +
                                      "&export=download&confirm=t");

    RemoteInfo info;
    try {
        info = probe(target, opt);
    } catch (const std::exception&) {
        return {url, std::nullopt};
    }

    std::string ctype = lower(info.content_type ? *info.content_type : "");
    if (ctype.find("text/html") == std::string::npos) {
        // The bytes themselves — use the name probe read from Content-Disposition.
        std::optional<std::string> name;
        if (!info.filename.empty()) name = info.filename;
        return {info.final_url, name};
    }

    // The small interstitial page: parse it for the real URL + name.
    std::string html;
    try {
        html = get_bytes(target, opt);
    } catch (const std::exception&) {
        return {info.final_url, std::nullopt};
    }

    auto name = name_from_html(html);
    if (auto parsed = parse_download_form(html)) return {*parsed, name};

    std::smatch m;
    if (std::regex_search(html, m, std::regex("confirm=([0-9A-Za-z_\\-]+)"))) {
        return {std::string(kDownload) + "?id=" + pct_encode(*fid) + "&export=download&confirm=" +
                    m[1].str(),
                name};
    }
    return {info.final_url, name};
}

} // namespace mpdm
