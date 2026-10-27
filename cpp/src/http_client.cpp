#include "mpdm/http_client.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <regex>
#include <vector>

namespace mpdm {

// ---- one-time libcurl global init ----------------------------------------
static void ensure_global_init() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// ---- filename helpers (ported from http_client.py) -----------------------
std::string sanitize_filename(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        unsigned char u = static_cast<unsigned char>(c);
        if (c == '\\' || c == '/') out.push_back('_');
        else if (u < 0x20 || std::strchr("<>:\"|?*", c)) out.push_back('_');
        else out.push_back(c);
    }
    // strip leading/trailing spaces and dots
    size_t a = out.find_first_not_of(" .");
    size_t b = out.find_last_not_of(" .");
    if (a == std::string::npos) return "download";
    out = out.substr(a, b - a + 1);
    return out.empty() ? "download" : out;
}

std::optional<std::string> filename_from_disposition(const std::string& cd) {
    if (cd.empty()) return std::nullopt;
    // filename*=charset'lang'<pct-encoded>
    std::smatch m;
    std::regex star(R"(filename\*\s*=\s*[^']*'[^']*'([^;]+))", std::regex::icase);
    if (std::regex_search(cd, m, star)) {
        std::string v = m[1].str();
        CURL* c = curl_easy_init();
        if (c) {
            int outlen = 0;
            char* dec = curl_easy_unescape(c, v.c_str(), (int)v.size(), &outlen);
            std::string r = dec ? std::string(dec, outlen) : v;
            if (dec) curl_free(dec);
            curl_easy_cleanup(c);
            // trim
            size_t s = r.find_first_not_of(" \t");
            size_t e = r.find_last_not_of(" \t");
            if (s != std::string::npos) return r.substr(s, e - s + 1);
        }
    }
    std::regex plain(R"(filename\s*=\s*\"?([^\";]+)\"?)", std::regex::icase);
    if (std::regex_search(cd, m, plain)) {
        std::string r = m[1].str();
        size_t s = r.find_first_not_of(" \t");
        size_t e = r.find_last_not_of(" \t");
        if (s != std::string::npos) return r.substr(s, e - s + 1);
    }
    return std::nullopt;
}

std::string filename_from_url(const std::string& url) {
    std::string name = "download";
    CURLU* u = curl_url();
    if (u && curl_url_set(u, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK) {
        char* path = nullptr;
        if (curl_url_get(u, CURLUPART_PATH, &path, 0) == CURLUE_OK && path) {
            std::string p(path);
            size_t slash = p.find_last_of('/');
            std::string base = (slash == std::string::npos) ? p : p.substr(slash + 1);
            if (!base.empty()) {
                CURL* c = curl_easy_init();
                if (c) {
                    int outlen = 0;
                    char* dec = curl_easy_unescape(c, base.c_str(), (int)base.size(), &outlen);
                    if (dec) { name = std::string(dec, outlen); curl_free(dec); }
                    curl_easy_cleanup(c);
                } else {
                    name = base;
                }
            }
            curl_free(path);
        }
    }
    if (u) curl_url_cleanup(u);
    return name.empty() ? "download" : name;
}

// ---- header collection ----------------------------------------------------
namespace {
struct Headers {
    // Reset on each new status line so we keep the FINAL response's headers.
    std::map<std::string, std::string> fields;  // lowercased name -> value
    int status = 0;
};

size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* h = static_cast<Headers*>(userdata);
    std::string line(buffer, size * nitems);
    // strip trailing CRLF
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.rfind("HTTP/", 0) == 0) {  // status line -> new block
        h->fields.clear();
        std::smatch m;
        std::regex re(R"(HTTP/\S+\s+(\d+))");
        if (std::regex_search(line, m, re)) h->status = std::stoi(m[1].str());
        return size * nitems;
    }
    auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string name = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        for (auto& c : name) c = (char)std::tolower((unsigned char)c);
        size_t s = val.find_first_not_of(" \t");
        if (s != std::string::npos) val = val.substr(s);
        h->fields[name] = val;
    }
    return size * nitems;
}

// Write callback that forwards to a std::function sink; return 0 aborts.
struct SinkCtx {
    const std::function<bool(const char*, size_t)>* sink;
    bool aborted = false;
};
size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* c = static_cast<SinkCtx*>(userdata);
    size_t n = size * nmemb;
    if (!(*c->sink)(ptr, n)) { c->aborted = true; return 0; }  // abort
    return n;
}

curl_slist* build_header_list(const HttpOptions& opt) {
    curl_slist* list = nullptr;
    list = curl_slist_append(list, "Accept: */*");
    for (const auto& kv : opt.headers) {
        if (kv.second.empty()) continue;
        list = curl_slist_append(list, (kv.first + ": " + kv.second).c_str());
    }
    return list;
}

void apply_common(CURL* c, const HttpOptions& opt, curl_slist* hdrs) {
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, opt.user_agent.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, opt.connect_timeout);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, opt.low_speed_limit);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, opt.low_speed_time);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 1L);
    if (!opt.proxy.empty()) curl_easy_setopt(c, CURLOPT_PROXY, opt.proxy.c_str());
    if (!opt.verify_tls) {
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    }
}
}  // namespace

// ---- probe ----------------------------------------------------------------
RemoteInfo probe(const std::string& url, const HttpOptions& opt) {
    ensure_global_init();
    CURL* c = curl_easy_init();
    if (!c) throw HttpError("curl init failed");
    curl_slist* hdrs = build_header_list(opt);
    Headers H;
    // Discard the body — we only want headers/status. Returning false after the
    // first bytes aborts so we never download a whole (range-ignoring) file.
    std::function<bool(const char*, size_t)> discard = [](const char*, size_t) { return false; };
    SinkCtx sctx{&discard};

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_RANGE, "0-0");
    apply_common(c, opt, hdrs);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &H);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sctx);

    CURLcode rc = curl_easy_perform(c);
    // Aborting the body yields CURLE_WRITE_ERROR / ABORTED — expected and fine.
    if (rc != CURLE_OK && rc != CURLE_WRITE_ERROR && rc != CURLE_ABORTED_BY_CALLBACK) {
        std::string err = curl_easy_strerror(rc);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(c);
        throw HttpError("probe failed: " + err);
    }

    long code = H.status;
    if (code == 0) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    char* eff = nullptr;
    curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
    std::string final_url = eff ? eff : url;
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    if (code >= 400 || code == 0)
        throw HttpError("probe HTTP status " + std::to_string(code));

    RemoteInfo info;
    info.url = url;
    info.final_url = final_url;

    auto get = [&](const char* k) -> std::optional<std::string> {
        auto it = H.fields.find(k);
        return it == H.fields.end() ? std::nullopt : std::optional<std::string>(it->second);
    };

    bool accept_ranges = false;
    std::optional<long long> size;
    if (code == 206) {
        accept_ranges = true;
        if (auto cr = get("content-range")) {  // bytes 0-0/12345
            std::smatch m;
            std::regex re(R"(/(\d+)\s*$)");
            if (std::regex_search(*cr, m, re)) size = std::stoll(m[1].str());
        }
    } else {  // 200: server ignored Range
        if (auto ar = get("accept-ranges")) {
            std::string v = *ar;
            for (auto& ch : v) ch = (char)std::tolower((unsigned char)ch);
            accept_ranges = (v == "bytes");
        }
        if (auto cl = get("content-length")) {
            try { size = std::stoll(*cl); } catch (...) {}
        }
    }
    info.accept_ranges = accept_ranges;
    info.size = size;

    std::string fname;
    if (auto cd = get("content-disposition")) {
        if (auto f = filename_from_disposition(*cd)) fname = *f;
    }
    if (fname.empty()) fname = filename_from_url(final_url);
    info.filename = sanitize_filename(fname);

    info.etag = get("etag");
    info.last_modified = get("last-modified");
    info.content_type = get("content-type");
    return info;
}

// ---- ranged GET -----------------------------------------------------------
long get_range(const std::string& url, long long start, long long end,
               const HttpOptions& opt,
               const std::function<bool(const char*, size_t)>& sink) {
    ensure_global_init();
    CURL* c = curl_easy_init();
    if (!c) throw HttpError("curl init failed");
    curl_slist* hdrs = build_header_list(opt);
    SinkCtx sctx{&sink};

    std::string range = std::to_string(start) + "-" + (end < 0 ? "" : std::to_string(end));
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_RANGE, range.c_str());
    apply_common(c, opt, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sctx);

    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    bool aborted = sctx.aborted;
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK && !(aborted && (rc == CURLE_WRITE_ERROR || rc == CURLE_ABORTED_BY_CALLBACK)))
        throw HttpError(std::string("get_range failed: ") + curl_easy_strerror(rc));
    return code;
}

std::string get_bytes(const std::string& url, const HttpOptions& opt) {
    std::string out;
    get_range(url, 0, -1, opt, [&](const char* d, size_t n) { out.append(d, n); return true; });
    return out;
}

std::string urljoin(const std::string& base, const std::string& rel) {
    CURLU* u = curl_url();
    if (!u) return rel;
    std::string result = rel;
    if (curl_url_set(u, CURLUPART_URL, base.c_str(), 0) == CURLUE_OK &&
        curl_url_set(u, CURLUPART_URL, rel.c_str(), 0) == CURLUE_OK) {
        char* out = nullptr;
        if (curl_url_get(u, CURLUPART_URL, &out, 0) == CURLUE_OK && out) {
            result = out;
            curl_free(out);
        }
    }
    curl_url_cleanup(u);
    return result;
}

} // namespace mpdm
