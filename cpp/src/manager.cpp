#include "mpdm/manager.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

#include <thread>

#include "mpdm/batch.hpp"
#include "mpdm/categories.hpp"
#include "mpdm/checksum.hpp"
#include "mpdm/grabber.hpp"
#include "mpdm/http_client.hpp"
#include "mpdm/storage.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace mpdm {

// ---- settings.json prefs ---------------------------------------------------

static std::string prefs_path() { return (fs::path(state_dir()) / "settings.json").string(); }

static json settings_to_json(const Settings& s) {
    return {{"connections", s.connections},
            {"min_segment_size", s.min_segment_size},
            {"connect_timeout", s.connect_timeout},
            {"read_timeout", s.read_timeout},
            {"max_retries", s.max_retries},
            {"retry_backoff", s.retry_backoff},
            {"retry_interval", s.retry_interval},
            {"user_agent", s.user_agent},
            {"output_dir", s.output_dir},
            {"verify_tls", s.verify_tls},
            {"proxy", s.proxy},
            {"max_rate_bps", s.max_rate_bps},
            {"categorize", s.categorize},
            {"category_dirs", s.category_dirs},
            {"category_exts", s.category_exts},
            {"remember_category_folder", s.remember_category_folder},
            {"temp_dir", s.temp_dir},
            {"set_file_times", s.set_file_times},
            {"dynamic_segmentation", s.dynamic_segmentation},
            {"duplicate_policy", s.duplicate_policy},
            {"resume_ignore_mtime", s.resume_ignore_mtime},
            {"auto_retry_minutes", s.auto_retry_minutes},
            {"auto_retry_hours", s.auto_retry_hours},
            {"resume_on_start", s.resume_on_start}};
}

static void apply_settings_json(Settings& s, const json& j) {
    if (!j.is_object()) return;
    s.connections = j.value("connections", s.connections);
    s.min_segment_size = j.value("min_segment_size", s.min_segment_size);
    s.connect_timeout = j.value("connect_timeout", s.connect_timeout);
    s.read_timeout = j.value("read_timeout", s.read_timeout);
    s.max_retries = j.value("max_retries", s.max_retries);
    s.retry_backoff = j.value("retry_backoff", s.retry_backoff);
    s.retry_interval = j.value("retry_interval", s.retry_interval);
    s.user_agent = j.value("user_agent", s.user_agent);
    s.output_dir = j.value("output_dir", s.output_dir);
    s.verify_tls = j.value("verify_tls", s.verify_tls);
    s.proxy = j.value("proxy", s.proxy);
    s.max_rate_bps = j.value("max_rate_bps", s.max_rate_bps);
    s.categorize = j.value("categorize", s.categorize);
    if (j.contains("category_dirs") && j["category_dirs"].is_object())
        s.category_dirs = j["category_dirs"].get<std::map<std::string, std::string>>();
    if (j.contains("category_exts") && j["category_exts"].is_object())
        s.category_exts = j["category_exts"].get<std::map<std::string, std::string>>();
    if (j.contains("remember_category_folder") && j["remember_category_folder"].is_object())
        s.remember_category_folder = j["remember_category_folder"].get<std::map<std::string, bool>>();
    s.temp_dir = j.value("temp_dir", s.temp_dir);
    s.set_file_times = j.value("set_file_times", s.set_file_times);
    s.dynamic_segmentation = j.value("dynamic_segmentation", s.dynamic_segmentation);
    s.duplicate_policy = j.value("duplicate_policy", s.duplicate_policy);
    s.resume_ignore_mtime = j.value("resume_ignore_mtime", s.resume_ignore_mtime);
    s.auto_retry_minutes = j.value("auto_retry_minutes", s.auto_retry_minutes);
    s.auto_retry_hours = j.value("auto_retry_hours", s.auto_retry_hours);
    s.resume_on_start = j.value("resume_on_start", s.resume_on_start);
}

static json load_prefs() {
    std::error_code ec;
    std::string p = prefs_path();
    if (!fs::exists(p, ec)) return json::object();
    try {
        std::ifstream f(p, std::ios::binary);
        json root;
        f >> root;
        return root;
    } catch (const std::exception&) {
        return json::object();
    }
}

Settings load_user_settings() {
    Settings s;  // defaults
    json prefs = load_prefs();
    if (prefs.contains("settings")) apply_settings_json(s, prefs["settings"]);
    apply_env(s);  // env wins (mirrors the manager ctor precedence)
    return s;
}

static double now_epoch() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

static std::string home_dir() {
    if (const char* h = std::getenv("USERPROFILE")) return h;
    if (const char* h = std::getenv("HOME")) return h;
    return ".";
}

std::string state_dir() {
    // MPDM_STATE_DIR isolates a run's state (downloads.json, jobs, settings) from
    // the user's real profile — used by dev/screenshot runs and tests so they never
    // pollute the installed app's queue.
    if (const char* s = std::getenv("MPDM_STATE_DIR"); s && *s) {
        fs::path d = s;
        std::error_code ec;
        fs::create_directories(d, ec);
        return d.string();
    }
    fs::path d = fs::path(home_dir()) / ".mypersonaldownloadmanager";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d.string();
}

static std::string gen_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    static const char* hex = "0123456789abcdef";
    std::string s(12, '0');
    for (char& c : s) c = hex[rng() & 0xF];
    return s;
}

std::string DownloadItem::category() const {
    return classify(!filename.empty() ? filename : url);
}

std::optional<double> DownloadItem::percent() const {
    if (!size || *size == 0) return std::nullopt;
    double p = static_cast<double>(downloaded) * 100.0 / static_cast<double>(*size);
    return p < 100.0 ? p : 100.0;
}

DownloadManager::DownloadManager(Settings s, int max_concurrent, long long global_rate_bps)
    : settings(std::move(s)),
      max_concurrent(max_concurrent),
      global_limiter_(static_cast<double>(global_rate_bps)),
      store_((fs::path(state_dir()) / "downloads.json").string()) {
    // Precedence: defaults (the passed Settings) -> settings.json -> environment.
    json prefs = load_prefs();
    if (prefs.contains("settings")) apply_settings_json(settings, prefs["settings"]);
    apply_env(settings);
    this->max_concurrent = prefs.value("max_concurrent", max_concurrent);
    global_limiter_.set_rate(prefs.value("global_rate_bps", static_cast<double>(global_rate_bps)));
    on_done_action = prefs.value("on_done_action", std::string("none"));
    on_done_command = prefs.value("on_done_command", std::string(""));
    clip_monitor_pref = prefs.value("clip_monitor", false);

    // Scheduler: created + loaded here, started only on start_scheduler().
    scheduler_ = std::make_unique<Scheduler>(
        std::map<std::string, Scheduler::Action>{
            {"pause_all", [this](const ScheduleRule&) { pause_all(); }},
            {"resume_all", [this](const ScheduleRule&) { resume_all(); }},
            {"set_global_rate",
             [this](const ScheduleRule& r) { set_global_rate(static_cast<double>(r.rate_bps)); }},
        },
        (fs::path(state_dir()) / "schedule.json").string());
    scheduler_->load();

    load();
    sweep_incognito();
}

DownloadManager::~DownloadManager() {
    if (scheduler_) scheduler_->stop();
    shutdown();
}

void DownloadManager::shutdown() {
    shutting_ = true;
    {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        for (auto& kv : items_)
            if (kv.second->dl) kv.second->dl->cancel();
    }
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    threads_.clear();
}

// ---- persistence ----------------------------------------------------------
static json item_to_json(const DownloadItem& it) {
    json j;
    j["id"] = it.id;
    j["url"] = it.url;
    j["filename"] = it.filename;
    j["dest_path"] = it.dest_path.empty() ? json(nullptr) : json(it.dest_path);
    j["size"] = it.size ? json(*it.size) : json(nullptr);
    j["downloaded"] = it.downloaded;
    j["status"] = it.status;
    j["connections"] = it.connections ? json(it.connections) : json(nullptr);
    j["max_rate_bps"] = it.max_rate_bps;
    j["incognito"] = it.incognito;
    j["output_dir"] = it.output_dir;
    j["proxy"] = it.proxy;
    j["stream"] = it.stream;
    j["headers"] = it.headers;
    j["error"] = it.error;
    j["added_at"] = it.added_at;
    j["failed_since"] = it.failed_since;
    j["failed_at"] = it.failed_at;
    j["no_auto_retry"] = it.no_auto_retry;
    j["expected_hash"] = it.expected_hash;
    j["hash_state"] = it.hash_state == "checking" ? "" : it.hash_state;  // a check doesn't survive a restart
    return j;
}

void DownloadManager::save() {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    json arr = json::array();
    for (const auto& id : order_) {
        auto it = items_.find(id);
        if (it == items_.end() || it->second->incognito) continue;  // incognito never persisted
        arr.push_back(item_to_json(*it->second));
    }
    json root;
    root["items"] = arr;
    try {
        std::string tmp = store_ + ".tmp";
        { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); f << root.dump(2); }
        fs::rename(tmp, store_);
    } catch (const std::exception&) {}
}

void DownloadManager::load() {
    std::error_code ec;
    if (!fs::exists(store_, ec)) return;
    try {
        std::ifstream f(store_, std::ios::binary);
        json root;
        f >> root;
        std::lock_guard<std::recursive_mutex> lk(mu_);
        for (const auto& j : root.value("items", json::array())) {
            auto it = std::make_shared<DownloadItem>();
            it->id = j.value("id", gen_id());
            it->url = j.value("url", "");
            it->filename = j.value("filename", "");
            if (j.contains("dest_path") && !j["dest_path"].is_null())
                it->dest_path = j["dest_path"].get<std::string>();
            if (j.contains("size") && !j["size"].is_null()) it->size = j["size"].get<long long>();
            it->downloaded = j.value("downloaded", 0LL);
            it->status = j.value("status", std::string(ST_PAUSED));
            if (j.contains("connections") && !j["connections"].is_null())
                it->connections = j["connections"].get<int>();
            it->max_rate_bps = j.value("max_rate_bps", 0LL);
            it->incognito = j.value("incognito", false);
            it->output_dir = j.value("output_dir", "");
            it->proxy = j.value("proxy", "");
            it->stream = j.value("stream", false);
            if (j.contains("headers")) it->headers = j["headers"].get<std::map<std::string, std::string>>();
            it->error = j.value("error", "");
            it->added_at = j.value("added_at", now_epoch());
            it->failed_since = j.value("failed_since", 0.0);
            it->failed_at = j.value("failed_at", 0.0);
            it->no_auto_retry = j.value("no_auto_retry", false);
            it->expected_hash = j.value("expected_hash", "");
            it->hash_state = j.value("hash_state", "");
            // Anything mid-flight comes back paused; resume_interrupted() continues
            // these when "resume on start" is on.
            if (it->status == ST_DOWNLOADING || it->status == ST_QUEUED) {
                it->status = ST_PAUSED;
                interrupted_.push_back(it->id);
            }
            items_[it->id] = it;
            order_.push_back(it->id);
        }
    } catch (const std::exception&) {}
}

// ---- listing --------------------------------------------------------------
std::vector<std::shared_ptr<DownloadItem>> DownloadManager::ordered() {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    std::vector<std::shared_ptr<DownloadItem>> out;
    for (const auto& id : order_) {
        auto it = items_.find(id);
        if (it != items_.end()) out.push_back(it->second);
    }
    return out;
}

int DownloadManager::active_count() {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    int n = 0;
    for (const auto& kv : items_)
        if (kv.second->status == ST_DOWNLOADING) ++n;
    return n;
}

// ---- mutations ------------------------------------------------------------
static bool url_is_stream(const std::string& url) {
    std::string low = url;
    for (auto& c : low) c = (char)::tolower((unsigned char)c);
    return low.find(".m3u8") != std::string::npos || low.find(".mpd") != std::string::npos;
}

std::shared_ptr<DownloadItem> DownloadManager::add(const std::string& url, const std::string& filename,
                                                   int connections,
                                                   std::map<std::string, std::string> headers,
                                                   bool start, long long max_rate_bps, bool incognito,
                                                   bool stream) {
    auto it = std::make_shared<DownloadItem>();
    it->id = gen_id();
    it->url = url;
    // trim
    size_t a = it->url.find_first_not_of(" \t\r\n");
    size_t b = it->url.find_last_not_of(" \t\r\n");
    it->url = (a == std::string::npos) ? "" : it->url.substr(a, b - a + 1);
    it->filename = filename;
    it->connections = connections;
    it->max_rate_bps = max_rate_bps > 0 ? max_rate_bps : 0;
    it->incognito = incognito;
    it->stream = stream || url_is_stream(it->url);
    it->headers = std::move(headers);
    it->status = start ? ST_QUEUED : ST_PAUSED;
    it->added_at = now_epoch();
    {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        items_[it->id] = it;
        order_.push_back(it->id);
    }
    save();
    if (start) pump();
    return it;
}

std::vector<std::shared_ptr<DownloadItem>> DownloadManager::add_batch(const std::string& text,
                                                                      int connections, bool start,
                                                                      long long max_rate_bps,
                                                                      bool incognito) {
    std::vector<std::shared_ptr<DownloadItem>> out;
    for (const auto& url : expand_urls(text))
        out.push_back(add(url, "", connections, {}, start, max_rate_bps, incognito));
    return out;
}

void DownloadManager::pump() {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    if (shutting_) return;
    int active = 0;
    for (const auto& kv : items_)
        if (kv.second->status == ST_DOWNLOADING) ++active;
    int slots = max_concurrent - active;
    for (const auto& id : order_) {
        if (slots <= 0) break;
        auto it = items_.find(id);
        if (it == items_.end() || it->second->status != ST_QUEUED) continue;
        auto item = it->second;
        item->status = ST_DOWNLOADING;
        on_complete_fired_ = false;  // new work -> allow the finish action to fire again
        --slots;
        threads_.emplace_back([this, item] { run(item); });
    }
}

void DownloadManager::run(std::shared_ptr<DownloadItem> item) {
    Settings s = settings;
    if (item->connections) s.connections = item->connections;
    if (item->max_rate_bps) s.max_rate_bps = item->max_rate_bps;
    if (!item->output_dir.empty()) s.output_dir = item->output_dir;  // per-item "Save to"
    if (!item->proxy.empty()) s.proxy = item->proxy;                 // per-item proxy
    if (item->incognito) {
        s.output_dir = ensure_hidden_dir((fs::path(s.output_dir) / INCOGNITO_DIRNAME).string());
        s.categorize = false;
    }

    // Stream (HLS/DASH) items run through the grabber -> MP4. grab() has no
    // pause/cancel, so these run to completion (the UI disables pause for them).
    if (item->stream) {
        std::string out = item->dest_path;
        if (out.empty()) {
            std::string fname = item->filename.empty() ? "video.mp4" : sanitize_filename(item->filename);
            std::string low = fname;
            for (auto& c : low) c = (char)::tolower((unsigned char)c);
            if (low.size() < 4 || (low.rfind(".mp4") != low.size() - 4 && low.rfind(".mkv") != low.size() - 4 &&
                                   low.rfind(".webm") != low.size() - 5))
                fname += ".mp4";
            std::string dir = categorized_dir(s.output_dir, fname, s.categorize, s.category_dirs, s.category_exts);
            std::error_code ec; fs::create_directories(dir, ec);
            out = unique_dest(dir, fname);
        }
        item->dest_path = out;
        item->filename = fs::path(out).filename().string();
        GrabProgressCb gcb = [item](int done, int total) {
            item->seg_done = done; item->seg_total = total;
            item->downloaded = done; if (total) item->size = total;
        };
        try {
            std::string path = grab(item->url, out, s, "auto", "best", gcb);
            item->dest_path = path;
            item->speed = 0; item->eta = 0;
            if (verify_hash(*item)) {
                item->status = ST_COMPLETED;
                item->failed_since = item->failed_at = 0;
            }
        } catch (const std::exception& e) {
            mark_failed(*item, e.what());
        }
        item->dl = nullptr;
        save(); pump(); maybe_on_complete();
        return;
    }

    auto cb = [item](const Progress& p) {
        item->downloaded = p.downloaded;
        if (p.total) item->size = *p.total;
        item->speed = p.speed_bps;
        item->eta = p.eta_seconds;
        item->conn_stats = p.connections;
        if (item->dl && !item->dl->dest_path().empty()) {
            item->dest_path = item->dl->target_path();
            if (item->filename.empty())
                item->filename = fs::path(item->dest_path).filename().string();
        }
    };

    // dest is set only for a resume / an explicit path. Otherwise pass the caller's
    // filename to the engine as a preference so the engine (not us) resolves the
    // folder AND applies the duplicate-file policy (rename / overwrite / skip / ask).
    std::string dest = item->dest_path;
    auto dl = std::make_shared<Download>(item->url, s, dest, cb, item->headers, &global_limiter_);
    if (dest.empty() && !item->filename.empty())
        dl->set_preferred_filename(sanitize_filename(item->filename));
    if (duplicate_resolver_) dl->set_duplicate_resolver(duplicate_resolver_);
    if (resume_resolver_) dl->set_resume_resolver(resume_resolver_);
    item->dl = dl;
    try {
        std::string path = dl->start();
        item->dest_path = path;
        item->filename = fs::path(path).filename().string();
        item->speed = 0;
        item->eta = 0;
        const bool good = verify_hash(*item);  // (still "downloading" while it hashes)
        if (good) {
            item->status = ST_COMPLETED;
            item->failed_since = item->failed_at = 0;
            item->no_auto_retry = false;
        }
        // "Remember the folder I last chose for this category": when on, update the
        // category's default folder to wherever this (non-incognito) download landed.
        if (good && !item->incognito) {
            std::string cat = classify(item->filename, settings.category_exts);
            auto rit = settings.remember_category_folder.find(cat);
            if (rit != settings.remember_category_folder.end() && rit->second) {
                std::string folder = fs::path(path).parent_path().string();
                if (!folder.empty() && settings.category_dirs[cat] != folder) {
                    settings.category_dirs[cat] = folder;
                    save_settings();
                }
            }
        }
    } catch (const ResumeConflictError& e) {
        mark_failed(*item, e.what(), /*no_auto_retry=*/true);  // the user chose to stop
    } catch (const DownloadError& e) {
        std::string msg = e.what();
        if (msg.find("paused") != std::string::npos) {
            item->status = ST_PAUSED;
            item->speed = 0;
        } else if (msg.find("cancelled") != std::string::npos) {
            item->dl = nullptr;
            return;  // removed — don't touch
        } else {
            mark_failed(*item, msg);
        }
    } catch (const std::exception& e) {
        mark_failed(*item, e.what());
    }
    item->dl = nullptr;
    save();
    pump();
    maybe_on_complete();
}

void DownloadManager::pause(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto it = items_.find(id);
    if (it == items_.end()) return;
    auto item = it->second;
    if (item->status == ST_DOWNLOADING && item->dl) {
        item->dl->pause();  // engine checkpoints + raises -> run() sets paused
    } else if (item->status == ST_QUEUED) {
        item->status = ST_PAUSED;
        save();
    }
}

void DownloadManager::resume(const std::string& id) {
    {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        auto it = items_.find(id);
        if (it == items_.end()) return;
        auto item = it->second;
        if (item->status != ST_PAUSED && item->status != ST_ERROR) return;
        item->error.clear();
        item->status = ST_QUEUED;
        // A manual Resume/Retry starts a fresh auto-retry window.
        item->failed_since = item->failed_at = 0;
        item->no_auto_retry = false;
        if (item->hash_state == "mismatch") item->hash_state.clear();
    }
    save();
    pump();
}

void DownloadManager::mark_failed(DownloadItem& it, const std::string& msg, bool no_auto_retry) {
    const double now = now_epoch();
    it.status = ST_ERROR;
    it.error = msg;
    it.speed = 0;
    if (it.failed_since <= 0) it.failed_since = now;
    it.failed_at = now;
    it.no_auto_retry = no_auto_retry;
}

static std::string mismatch_message(const std::string& algo) {
    return checksum_label(algo) + " checksum doesn't match: the file is damaged or isn't the expected one";
}

bool DownloadManager::verify_hash(DownloadItem& it) {
    Checksum want = parse_checksum(it.expected_hash);
    if (!want.valid()) return true;
    it.hash_state = "checking";
    std::string got = file_digest(it.dest_path, want.algo, &shutting_);
    if (got.empty()) {  // unreadable, or the app is closing: leave it unchecked
        it.hash_state.clear();
        return true;
    }
    if (got == want.hex) {
        it.hash_state = "ok";
        return true;
    }
    it.hash_state = "mismatch";
    mark_failed(it, mismatch_message(want.algo), /*no_auto_retry=*/true);
    return false;
}

bool DownloadManager::set_expected_hash(const std::string& id, const std::string& text) {
    const bool clear = text.find_first_not_of(" \t\r\n") == std::string::npos;
    Checksum c = parse_checksum(text);
    if (!clear && !c.valid()) return false;
    {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        auto f = items_.find(id);
        if (f == items_.end() || f->second->hash_state == "checking") return false;
        auto item = f->second;
        item->expected_hash = c.str();
        item->hash_state.clear();
        // Already finished: check the file on disk now, off the caller's thread.
        if (!clear && item->status == ST_COMPLETED && !item->dest_path.empty() && !shutting_) {
            item->hash_state = "checking";
            threads_.emplace_back([this, item, c] {
                std::string got = file_digest(item->dest_path, c.algo, &shutting_);
                {
                    std::lock_guard<std::recursive_mutex> lk2(mu_);
                    if (got.empty()) item->hash_state.clear();
                    else if (got == c.hex) item->hash_state = "ok";
                    else {
                        item->hash_state = "mismatch";
                        mark_failed(*item, mismatch_message(c.algo), /*no_auto_retry=*/true);
                    }
                }
                save();
            });
        }
    }
    save();
    return true;
}

double DownloadManager::auto_retry_in(const DownloadItem& it, double now) {
    if (now <= 0) now = now_epoch();
    if (it.status != ST_ERROR || it.no_auto_retry || settings.auto_retry_minutes <= 0 || it.failed_at <= 0)
        return -1;
    const double since = it.failed_since > 0 ? it.failed_since : it.failed_at;
    if (settings.auto_retry_hours > 0 && now - since >= settings.auto_retry_hours * 3600.0) return -1;  // gave up
    return std::max(0.0, it.failed_at + settings.auto_retry_minutes * 60.0 - now);
}

int DownloadManager::retry_failed_due(double now) {
    if (now <= 0) now = now_epoch();
    int n = 0;
    {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        if (shutting_) return 0;
        for (auto& kv : items_) {
            auto& it = *kv.second;
            if (auto_retry_in(it, now) != 0.0) continue;  // -1 = never, >0 = not yet
            it.status = ST_QUEUED;  // keeps failed_since: the give-up clock keeps running
            it.error.clear();
            ++n;
        }
    }
    if (n) { save(); pump(); }
    return n;
}

int DownloadManager::resume_interrupted() {
    int n = 0;
    {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        std::vector<std::string> ids;
        ids.swap(interrupted_);  // once per run
        if (!settings.resume_on_start) return 0;
        for (const auto& id : ids) {
            auto f = items_.find(id);
            if (f == items_.end() || f->second->status != ST_PAUSED) continue;  // paused by the user since
            f->second->status = ST_QUEUED;
            ++n;
        }
    }
    if (n) { save(); pump(); }
    return n;
}

void DownloadManager::delete_files(const std::string& dest_path) {
    if (dest_path.empty()) return;
    std::error_code ec;
    for (const std::string& p : {dest_path, sidecar_path(dest_path), dest_path + ".tmp"})
        fs::remove(p, ec);
}

void DownloadManager::remove(const std::string& id, bool delete_file) {
    std::shared_ptr<DownloadItem> item;
    bool active = false;
    {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        auto it = items_.find(id);
        if (it == items_.end()) return;
        item = it->second;
        active = (item->status == ST_DOWNLOADING && item->dl != nullptr);
        if (active) item->dl->cancel();
        items_.erase(it);
        order_.erase(std::remove(order_.begin(), order_.end(), id), order_.end());
    }
    if ((delete_file || item->incognito) && !item->dest_path.empty()) {
        if (active) {
            std::thread([this, item] {
                for (int i = 0; i < 60 && item->dl != nullptr; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                delete_files(item->dest_path);
            }).detach();
        } else {
            delete_files(item->dest_path);
        }
    }
    save();
    pump();
}

void DownloadManager::clear_finished() {
    {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        for (auto it = items_.begin(); it != items_.end();) {
            if (it->second->status == ST_COMPLETED) {
                std::string id = it->first;
                order_.erase(std::remove(order_.begin(), order_.end(), id), order_.end());
                it = items_.erase(it);
            } else {
                ++it;
            }
        }
    }
    save();
}

void DownloadManager::pause_all() {
    for (auto& item : ordered()) pause(item->id);
}

void DownloadManager::resume_all() {
    {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        for (auto& kv : items_) {
            if (kv.second->status == ST_PAUSED || kv.second->status == ST_ERROR) {
                kv.second->error.clear();
                kv.second->status = ST_QUEUED;
            }
        }
    }
    save();
    pump();
}

bool DownloadManager::set_url(const std::string& id, const std::string& url) {
    {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        auto it = items_.find(id);
        if (it == items_.end() || url.empty()) return false;
        auto item = it->second;
        if (item->status == ST_DOWNLOADING || item->status == ST_COMPLETED || item->dl) return false;
        item->url = url;
        item->stream = item->stream || url_is_stream(url);
        item->error.clear();
    }
    save();
    return true;
}

void DownloadManager::set_item_rate(const std::string& id, long long bps) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    auto it = items_.find(id);
    if (it == items_.end()) return;
    it->second->max_rate_bps = bps > 0 ? bps : 0;
    if (it->second->dl) it->second->dl->set_rate(static_cast<double>(it->second->max_rate_bps));
    save();
}

// ---- settings persistence --------------------------------------------------
void DownloadManager::save_settings() {
    json prefs = {{"settings", settings_to_json(settings)},
                  {"max_concurrent", max_concurrent},
                  {"global_rate_bps", global_rate_bps()},
                  {"on_done_action", on_done_action},
                  {"on_done_command", on_done_command},
                  {"clip_monitor", clip_monitor_pref}};
    try {
        std::string p = prefs_path();
        std::string tmp = p + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f << prefs.dump(2);
        }
        fs::rename(tmp, p);
    } catch (const std::exception&) {
    }
}

// ---- "when everything finishes" action -------------------------------------
void DownloadManager::set_on_complete(const std::string& action, const std::string& command) {
    on_done_action = (action == "run" || action == "sleep" || action == "shutdown") ? action : "none";
    on_done_command = command;
    on_complete_fired_ = false;
}

bool DownloadManager::queue_all_done() {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    bool any_completed = false;
    for (const auto& kv : items_) {
        const std::string& st = kv.second->status;
        if (st == ST_DOWNLOADING || st == ST_QUEUED) return false;
        if (st == ST_COMPLETED) any_completed = true;
    }
    return any_completed;
}

void DownloadManager::maybe_on_complete() {
    if (on_done_action == "none") return;
    std::string action, cmd;
    {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        if (on_complete_fired_ || !queue_all_done()) return;
        on_complete_fired_ = true;
        action = on_done_action;
        cmd = on_done_command;
    }
    run_completion_action(action, cmd);
}

void DownloadManager::run_completion_action(const std::string& action, const std::string& command) {
    try {
#ifdef _WIN32
        if (action == "run" && !command.empty()) {
            std::system(("start \"\" " + command).c_str());
        } else if (action == "shutdown") {
            std::system("shutdown /s /t 60");  // cancel with: shutdown /a
        } else if (action == "sleep") {
            std::system("rundll32.exe powrprof.dll,SetSuspendState 0,1,0");
        }
#elif defined(__APPLE__)
        if (action == "run" && !command.empty()) std::system((command + " &").c_str());
        else if (action == "shutdown")
            std::system("osascript -e 'tell application \"System Events\" to shut down' &");
        else if (action == "sleep") std::system("pmset sleepnow &");
#else
        if (action == "run" && !command.empty()) std::system((command + " &").c_str());
        else if (action == "shutdown") std::system("shutdown -h +1 &");
        else if (action == "sleep") std::system("systemctl suspend &");
#endif
    } catch (...) {
    }
}

// ---- scheduler wrappers ----------------------------------------------------
void DownloadManager::start_scheduler() { if (scheduler_) scheduler_->start(); }
void DownloadManager::stop_scheduler() { if (scheduler_) scheduler_->stop(); }

ScheduleRule DownloadManager::add_schedule(const std::string& action, const std::string& at,
                                           const std::string& repeat, std::vector<int> days,
                                           long long rate_bps) {
    return scheduler_->add_rule(action, at, repeat, std::move(days), rate_bps);
}
void DownloadManager::remove_schedule(const std::string& id) { scheduler_->remove_rule(id); }
void DownloadManager::set_schedule_enabled(const std::string& id, bool enabled) {
    scheduler_->set_enabled(id, enabled);
}
std::vector<ScheduleRule> DownloadManager::list_schedules() { return scheduler_->list_rules(); }

// ---- incognito lifecycle ---------------------------------------------------
void DownloadManager::sweep_incognito() {
    std::string d = (fs::path(settings.output_dir) / INCOGNITO_DIRNAME).string();
    std::error_code ec;
    if (fs::is_directory(d, ec)) {
        try {
            clean_dir(d, /*purge_partials=*/true);
        } catch (const std::exception&) {
        }
    }
}

void DownloadManager::wait_dl_release(const std::shared_ptr<DownloadItem>& it) {
    for (int i = 0; i < 60 && it->dl != nullptr; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void DownloadManager::purge_incognito_incomplete() {
    for (auto& it : ordered()) {
        if (!it->incognito || it->status == ST_COMPLETED) continue;
        bool active = (it->status == ST_DOWNLOADING && it->dl != nullptr);
        if (active) it->dl->cancel();
        {
            std::lock_guard<std::recursive_mutex> lk(mu_);
            items_.erase(it->id);
            order_.erase(std::remove(order_.begin(), order_.end(), it->id), order_.end());
        }
        if (active) wait_dl_release(it);
        delete_files(it->dest_path);
    }
    save();
}

// ---- browser integration ---------------------------------------------------
// The native host drops a browser download request as jobs/<id>.json; the app
// adopts each into the queue so it downloads in-app and shows in the list.
std::vector<std::string> DownloadManager::ingest_jobs() {
    std::vector<std::string> ids;
    std::string dir = (fs::path(state_dir()) / "jobs").string();
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return ids;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (de.path().extension() != ".json") continue;
        json j;
        try {
            std::ifstream f(de.path(), std::ios::binary);
            f >> j;
        } catch (...) {
            fs::remove(de.path(), ec);
            continue;
        }
        fs::remove(de.path(), ec);  // consume before adding (don't re-ingest)
        std::string url = j.value("url", "");
        if (url.empty()) continue;
        std::string filename = j.value("filename", "");
        int conns = 0;
        if (j.contains("connections") && j["connections"].is_number()) conns = j["connections"].get<int>();
        std::map<std::string, std::string> headers;
        if (j.contains("headers") && j["headers"].is_object())
            for (auto it = j["headers"].begin(); it != j["headers"].end(); ++it)
                if (it.value().is_string()) headers[it.key()] = it.value().get<std::string>();
        bool stream = j.value("stream", false);
        bool incognito = j.value("incognito", false);
        long long rate = 0;
        if (j.contains("rate") && j["rate"].is_number()) rate = j["rate"].get<long long>();
        // Add paused so per-item overrides are set before the worker thread reads
        // them (run() snapshots settings at the top), then pump once below.
        auto item = add(url, filename, conns, std::move(headers), /*start=*/false, rate, incognito, stream);
        if (item) {
            item->output_dir = j.value("output_dir", "");
            item->proxy = j.value("proxy", "");
            item->status = ST_QUEUED;
            ids.push_back(item->id);
        }
    }
    if (!ids.empty()) { save(); pump(); }
    return ids;
}

} // namespace mpdm
