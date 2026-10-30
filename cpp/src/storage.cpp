#include "mpdm/storage.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace mpdm {

std::string sidecar_path(const std::string& dest_path) { return dest_path + ".mpdm"; }

static json to_json(const TaskRecord& rec) {
    json segs = json::array();
    for (const auto& s : rec.segments) {
        segs.push_back({{"index", s.index},
                        {"start", s.start},
                        {"end", s.end},
                        {"downloaded", s.downloaded}});
    }
    json j;
    j["url"] = rec.url;
    j["dest_path"] = rec.dest_path;
    j["size"] = rec.size ? json(*rec.size) : json(nullptr);
    j["accept_ranges"] = rec.accept_ranges;
    j["segments"] = segs;
    j["status"] = rec.status;
    j["etag"] = rec.etag ? json(*rec.etag) : json(nullptr);
    j["last_modified"] = rec.last_modified ? json(*rec.last_modified) : json(nullptr);
    return j;
}

void save_record(const std::string& dest_path, const TaskRecord& rec) {
    std::string side = sidecar_path(dest_path);
    std::string tmp = side + ".tmp";
    try {
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f << to_json(rec).dump(2);
        }
        fs::rename(tmp, side);   // atomic replace
    } catch (const std::exception&) {
        std::error_code ec;
        fs::remove(tmp, ec);
    }
}

std::optional<TaskRecord> load_record(const std::string& dest_path) {
    std::string side = sidecar_path(dest_path);
    std::error_code ec;
    if (!fs::exists(side, ec)) return std::nullopt;
    try {
        std::ifstream f(side, std::ios::binary);
        json j;
        f >> j;
        TaskRecord rec;
        rec.url = j.value("url", "");
        rec.dest_path = j.value("dest_path", dest_path);
        if (j.contains("size") && !j["size"].is_null()) rec.size = j["size"].get<long long>();
        rec.accept_ranges = j.value("accept_ranges", false);
        rec.status = j.value("status", "");
        if (j.contains("etag") && !j["etag"].is_null()) rec.etag = j["etag"].get<std::string>();
        if (j.contains("last_modified") && !j["last_modified"].is_null())
            rec.last_modified = j["last_modified"].get<std::string>();
        if (j.contains("segments")) {
            for (const auto& s : j["segments"]) {
                Segment seg;
                seg.index = s.value("index", 0);
                seg.start = s.value("start", 0LL);
                seg.end = s.value("end", -1LL);
                seg.downloaded = s.value("downloaded", 0LL);
                rec.segments.push_back(seg);
            }
        }
        return rec;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void clear_record(const std::string& dest_path) {
    std::error_code ec;
    fs::remove(sidecar_path(dest_path), ec);
}

std::optional<std::string> find_resumable(const std::string& dir, const std::string& url,
                                          std::optional<long long> size,
                                          std::optional<std::string> etag) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return std::nullopt;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (entry.path().extension() != ".mpdm") continue;
        std::string dest = entry.path().string();
        dest = dest.substr(0, dest.size() - 5);  // strip ".mpdm"
        auto rec = load_record(dest);
        if (!rec) continue;
        if (rec->url == url && rec->size == size && rec->etag == etag && fs::exists(dest, ec))
            return dest;
    }
    return std::nullopt;
}

std::string unique_dest(const std::string& dir, const std::string& filename) {
    // A name is "taken" only by a real, complete file the user already has — i.e. a
    // file with NO .mpdm sidecar. A file that still has a sidecar is MPDM's own
    // unfinished download, which we reuse (resume/overwrite) rather than bump to
    // "(1)"; an orphan sidecar with no file is just stale and ignored. So "(1)" is
    // added only on a genuine duplicate.
    auto taken = [](const fs::path& p) {
        std::error_code ec;
        return fs::exists(p, ec) && !fs::exists(sidecar_path(p.string()), ec);
    };
    fs::path base = fs::path(dir) / filename;
    if (!taken(base)) return base.string();
    std::string stem = fs::path(filename).stem().string();
    std::string ext = fs::path(filename).extension().string();
    for (int i = 1;; ++i) {
        fs::path cand = fs::path(dir) / (stem + " (" + std::to_string(i) + ")" + ext);
        if (!taken(cand)) return cand.string();
    }
}

CleanResult clean_dir(const std::string& dir, bool purge_partials) {
    CleanResult res;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return res;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        const fs::path& p = entry.path();
        std::string ext = p.extension().string();
        if (ext == ".mpdm") {
            std::string side = p.string();
            std::string dest = side.substr(0, side.size() - 5);  // strip ".mpdm"
            bool incomplete = true;
            if (auto rec = load_record(dest)) {
                incomplete = rec->segments.empty() ||
                             !std::all_of(rec->segments.begin(), rec->segments.end(),
                                          [](const Segment& s) { return s.complete(); });
            }
            if (purge_partials && incomplete && fs::exists(dest, ec)) {
                std::error_code rec_ec;
                if (fs::remove(dest, rec_ec))
                    res.partials.push_back(dest);
                else
                    res.kept_locked.push_back(dest);
            }
            std::error_code side_ec;
            if (fs::remove(side, side_ec))
                res.sidecars.push_back(side);
            else
                res.kept_locked.push_back(side);
        } else if (ext == ".tmp") {
            std::error_code tmp_ec;
            if (fs::remove(p, tmp_ec))
                res.tmp.push_back(p.string());
            else
                res.kept_locked.push_back(p.string());
        }
    }
    return res;
}

std::string ensure_hidden_dir(const std::string& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
#ifdef _WIN32
    SetFileAttributesA(path.c_str(), FILE_ATTRIBUTE_HIDDEN);
#endif
    return path;
}

} // namespace mpdm
