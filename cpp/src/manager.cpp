#include "mpdm/manager.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

#include "mpdm/batch.hpp"
#include "mpdm/categories.hpp"
#include "mpdm/storage.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace mpdm {

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
    load();
}

DownloadManager::~DownloadManager() { shutdown(); }

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

} // namespace mpdm
