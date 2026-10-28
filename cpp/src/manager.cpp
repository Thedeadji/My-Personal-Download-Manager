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
            {"user_agent", s.user_agent},
            {"output_dir", s.output_dir},
            {"verify_tls", s.verify_tls},
            {"proxy", s.proxy},
            {"max_rate_bps", s.max_rate_bps},
            {"categorize", s.categorize},
            {"category_dirs", s.category_dirs},
            {"dynamic_segmentation", s.dynamic_segmentation},
            {"duplicate_policy", s.duplicate_policy}};
}

static void apply_settings_json(Settings& s, const json& j) {
    if (!j.is_object()) return;
    s.connections = j.value("connections", s.connections);
    s.min_segment_size = j.value("min_segment_size", s.min_segment_size);
    s.connect_timeout = j.value("connect_timeout", s.connect_timeout);
    s.read_timeout = j.value("read_timeout", s.read_timeout);
    s.max_retries = j.value("max_retries", s.max_retries);
    s.retry_backoff = j.value("retry_backoff", s.retry_backoff);
    s.user_agent = j.value("user_agent", s.user_agent);
    s.output_dir = j.value("output_dir", s.output_dir);
    s.verify_tls = j.value("verify_tls", s.verify_tls);
    s.proxy = j.value("proxy", s.proxy);
    s.max_rate_bps = j.value("max_rate_bps", s.max_rate_bps);
    s.categorize = j.value("categorize", s.categorize);
    if (j.contains("category_dirs") && j["category_dirs"].is_object())
        s.category_dirs = j["category_dirs"].get<std::map<std::string, std::string>>();
    s.dynamic_segmentation = j.value("dynamic_segmentation", s.dynamic_segmentation);
    s.duplicate_policy = j.value("duplicate_policy", s.duplicate_policy);
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
    j["headers"] = it.headers;
    j["error"] = it.error;
    j["added_at"] = it.added_at;
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
            if (j.contains("headers")) it->headers = j["headers"].get<std::map<std::string, std::string>>();
            it->error = j.value("error", "");
            it->added_at = j.value("added_at", now_epoch());
            // Anything mid-flight comes back paused; never auto-resumes.
            if (it->status == ST_DOWNLOADING || it->status == ST_QUEUED) it->status = ST_PAUSED;
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
std::shared_ptr<DownloadItem> DownloadManager::add(const std::string& url, const std::string& filename,
                                                   int connections,
                                                   std::map<std::string, std::string> headers,
                                                   bool start, long long max_rate_bps, bool incognito) {
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
    if (item->incognito) {
        s.output_dir = ensure_hidden_dir((fs::path(s.output_dir) / INCOGNITO_DIRNAME).string());
        s.categorize = false;
    }

    auto cb = [item](const Progress& p) {
        item->downloaded = p.downloaded;
        if (p.total) item->size = *p.total;
        item->speed = p.speed_bps;
        item->eta = p.eta_seconds;
        item->conn_stats = p.connections;
        if (item->dl && !item->dl->dest_path().empty()) {
            item->dest_path = item->dl->dest_path();
            if (item->filename.empty())
                item->filename = fs::path(item->dl->dest_path()).filename().string();
        }
    };

    auto dl = std::make_shared<Download>(item->url, s, item->dest_path, cb, item->headers,
                                         &global_limiter_);
    item->dl = dl;
    try {
        std::string path = dl->start();
        item->dest_path = path;
        item->filename = fs::path(path).filename().string();
        item->status = ST_COMPLETED;
        item->speed = 0;
        item->eta = 0;
    } catch (const DownloadError& e) {
        std::string msg = e.what();
        if (msg.find("paused") != std::string::npos) {
            item->status = ST_PAUSED;
        } else if (msg.find("cancelled") != std::string::npos) {
            item->dl = nullptr;
            return;  // removed — don't touch
        } else {
            item->status = ST_ERROR;
            item->error = msg;
        }
        item->speed = 0;
    } catch (const std::exception& e) {
        item->status = ST_ERROR;
        item->error = e.what();
        item->speed = 0;
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
    }
    save();
    pump();
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
static void item_from_record(DownloadItem& it, const json& j) {
    it.id = j.value("id", gen_id());
    it.url = j.value("url", "");
    it.filename = j.value("filename", "");
    if (j.contains("dest_path") && !j["dest_path"].is_null())
        it.dest_path = j["dest_path"].get<std::string>();
    if (j.contains("size") && !j["size"].is_null()) it.size = j["size"].get<long long>();
    it.downloaded = j.value("downloaded", 0LL);
    it.status = j.value("status", std::string(ST_COMPLETED));
    if (j.contains("connections") && j["connections"].is_number())
        it.connections = j["connections"].get<int>();
    it.max_rate_bps = j.value("max_rate_bps", 0LL);
    if (j.contains("headers") && j["headers"].is_object())
        it.headers = j["headers"].get<std::map<std::string, std::string>>();
    it.error = j.value("error", "");
    it.added_at = j.value("added_at", now_epoch());
}

void DownloadManager::ingest_history_inbox() {
    std::string dir = (fs::path(state_dir()) / "history_inbox").string();
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    bool changed = false;
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
        std::string id = j.value("id", "");
        {
            std::lock_guard<std::recursive_mutex> lk(mu_);
            if (!id.empty() && items_.count(id) == 0) {
                auto it = std::make_shared<DownloadItem>();
                item_from_record(*it, j);
                items_[it->id] = it;
                order_.push_back(it->id);
                changed = true;
            }
        }
        fs::remove(de.path(), ec);  // consumed
    }
    if (changed) save();
}

std::vector<DownloadItem> read_active_browser() {
    std::vector<DownloadItem> out;
    std::string dir = (fs::path(state_dir()) / "active").string();
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (de.path().extension() != ".json") continue;
        json j;
        try {
            std::ifstream f(de.path(), std::ios::binary);
            f >> j;
        } catch (...) {
            continue;
        }
        DownloadItem it;
        it.id = j.value("id", "");
        it.url = j.value("url", "");
        it.filename = j.value("filename", "");
        if (j.contains("dest_path") && j["dest_path"].is_string()) it.dest_path = j["dest_path"].get<std::string>();
        bool stream = j.value("stream", false);
        if (stream) {
            long long t = j.value("seg_total", 0);
            if (t > 0) it.size = t;
            it.downloaded = j.value("seg_done", 0);
        } else {
            if (j.contains("total") && !j["total"].is_null()) it.size = j["total"].get<long long>();
            it.downloaded = j.value("downloaded", 0LL);
        }
        it.speed = j.value("speed", 0.0);
        std::string st = j.value("status", "downloading");
        it.status = st == "paused" ? ST_PAUSED
                    : st == "completed" ? ST_COMPLETED
                    : (st == "failed" || st == "stopped") ? ST_ERROR
                    : ST_DOWNLOADING;
        it.error = j.value("error", "");
        if (j.contains("connections") && j["connections"].is_array())
            for (const auto& c : j["connections"]) {
                SegmentProgress sp;
                sp.index = c.value("index", 0);
                sp.start = c.value("start", 0LL);
                sp.end = c.value("end", -1LL);
                sp.downloaded = c.value("downloaded", 0LL);
                sp.speed_bps = c.value("speed_bps", 0.0);
                sp.active = c.value("active", false);
                sp.stole_from = c.value("stole_from", -1);
                it.conn_stats.push_back(sp);
            }
        out.push_back(std::move(it));
    }
    return out;
}

} // namespace mpdm
