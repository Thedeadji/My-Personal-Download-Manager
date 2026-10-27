#include "mpdm/grabber.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>

#include "mpdm/aes128.hpp"
#include "mpdm/dash.hpp"
#include "mpdm/http_client.hpp"
#include "mpdm/muxer.hpp"

namespace fs = std::filesystem;

namespace mpdm {

static HttpOptions opts_from(const Settings& s) {
    HttpOptions o;
    o.user_agent = s.user_agent;
    o.proxy = s.proxy;
    o.connect_timeout = s.connect_timeout;
    o.low_speed_time = s.read_timeout;
    o.low_speed_limit = 1;
    o.verify_tls = s.verify_tls;
    return o;
}

// Fetch one segment to `path`, retrying, then optionally AES-128-CBC decrypt it.
static void download_one(const std::string& url, const std::string& path, const Settings& settings,
                         const HttpOptions& opt, const DecryptEntry& key_iv) {
    int attempt = 0;
    for (;;) {
        try {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            if (!f) throw std::runtime_error("cannot open segment file: " + path);
            long status = get_range(url, 0, -1, opt,
                                    [&](const char* data, size_t n) {
                                        f.write(data, static_cast<std::streamsize>(n));
                                        return static_cast<bool>(f);
                                    });
            f.close();
            if (status >= 400) throw HttpError("HTTP " + std::to_string(status) + " for " + url);

            if (key_iv) {
                std::ifstream in(path, std::ios::binary);
                std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                in.close();
                std::string clear = aes128_cbc_decrypt(key_iv->first, key_iv->second, data);
                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                out.write(clear.data(), static_cast<std::streamsize>(clear.size()));
            }
            return;
        } catch (const std::exception&) {
            if (++attempt > settings.max_retries) throw;
        }
    }
}

std::vector<DecryptEntry> hls_decrypt_plan(const MediaPlaylist& media, const HttpOptions& opt) {
    std::vector<DecryptEntry> plan;
    if (!media.is_encrypted()) return plan;  // empty = nothing to decrypt

    for (const auto& seg : media.seg_infos) {
        if (seg.key && seg.key->method != "NONE" && !seg.key->method.empty() &&
            seg.key->method != "AES-128") {
            throw std::runtime_error("HLS encryption method '" + seg.key->method +
                                     "' is not supported (only AES-128); SAMPLE-AES is unsupported");
        }
    }

    std::map<std::string, std::string> key_cache;
    plan.reserve(media.seg_infos.size());
    for (const auto& seg : media.seg_infos) {
        if (!seg.key || seg.key->method == "NONE" || seg.key->method.empty()) {
            plan.emplace_back(std::nullopt);
            continue;
        }
        if (!seg.key->uri) throw std::runtime_error("encrypted segment has no key URI");
        auto it = key_cache.find(*seg.key->uri);
        if (it == key_cache.end())
            it = key_cache.emplace(*seg.key->uri, fetch_key(*seg.key->uri, opt)).first;
        plan.emplace_back(std::make_pair(it->second, segment_iv(seg)));
    }
    return plan;
}

// Download all segments concurrently (bounded by settings.connections).
static std::vector<std::string> fetch_segments(const std::vector<std::string>& seg_urls,
                                               const fs::path& workdir, const Settings& settings,
                                               const HttpOptions& opt,
                                               const GrabProgressCb& progress_cb,
                                               const std::vector<DecryptEntry>& plan) {
    int total = static_cast<int>(seg_urls.size());
    std::vector<std::string> paths(total);
    for (int i = 0; i < total; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "seg_%06d", i);
        paths[i] = (workdir / name).string();
    }

    std::atomic<int> next{0};
    std::atomic<int> done{0};
    std::mutex err_mu;
    std::exception_ptr first_err;

    int nworkers = std::min<int>(settings.connections, std::max(1, total));
    auto worker = [&]() {
        for (;;) {
            int i = next.fetch_add(1);
            if (i >= total) return;
            if (first_err) return;
            try {
                DecryptEntry ki = plan.empty() ? DecryptEntry{std::nullopt} : plan[i];
                download_one(seg_urls[i], paths[i], settings, opt, ki);
            } catch (...) {
                std::lock_guard<std::mutex> lk(err_mu);
                if (!first_err) first_err = std::current_exception();
                return;
            }
            int d = done.fetch_add(1) + 1;
            if (progress_cb) progress_cb(d, total);
        }
    };

    std::vector<std::thread> pool;
    for (int i = 0; i < nworkers; ++i) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    if (first_err) std::rethrow_exception(first_err);
    return paths;
}

static fs::path make_workdir() {
    std::random_device rd;
    for (int attempt = 0; attempt < 16; ++attempt) {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        fs::path dir = fs::temp_directory_path() /
                       ("mdm_grab_" + std::to_string(stamp) + "_" + std::to_string(rd()));
        std::error_code ec;
        if (fs::create_directory(dir, ec)) return dir;
    }
    throw std::runtime_error("could not create a temp work directory");
}

std::string grab(const std::string& url, const std::string& out_path, const Settings& settings,
                 const std::string& kind_in, const std::string& prefer,
                 const GrabProgressCb& progress_cb) {
    HttpOptions opt = opts_from(settings);

    std::string kind = kind_in;
    if (kind == "auto") {
        std::string low = url;
        for (auto& c : low) c = static_cast<char>(::tolower(c));
        kind = low.find(".mpd") != std::string::npos ? "dash" : "hls";
    }

    fs::path workdir = make_workdir();
    std::optional<std::string> init_path;
    try {
        std::vector<std::string> seg_urls;
        std::optional<std::string> init_url;
        std::vector<DecryptEntry> plan;

        if (kind == "hls") {
            auto [media, media_url] = resolve_media_playlist(url, opt, prefer);
            (void)media_url;
            if (media.segments.empty())
                throw std::runtime_error("no media segments found in HLS playlist");
            plan = hls_decrypt_plan(media, opt);
            seg_urls = media.segments;
            init_url = media.init_segment;
        } else if (kind == "dash") {
            DashStream stream = parse_mpd(url, opt, prefer);
            seg_urls = stream.segment_urls;
            init_url = stream.init_url;
            if (seg_urls.empty()) throw std::runtime_error("no segments found in MPD");
        } else {
            throw std::runtime_error("unknown kind: " + kind);
        }

        if (init_url) {
            init_path = (workdir / "init.mp4").string();
            download_one(*init_url, *init_path, settings, opt, std::nullopt);
        }

        auto seg_paths = fetch_segments(seg_urls, workdir, settings, opt, progress_cb, plan);

        fs::path out_abs = fs::absolute(out_path);
        std::error_code ec;
        fs::create_directories(out_abs.parent_path(), ec);
        assemble_segments(seg_paths, out_path, init_path, /*cleanup=*/true);

        std::error_code ec2;
        fs::remove_all(workdir, ec2);
        return out_path;
    } catch (...) {
        std::error_code ec;
        fs::remove_all(workdir, ec);
        throw;
    }
}

} // namespace mpdm
