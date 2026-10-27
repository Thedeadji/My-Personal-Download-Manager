#include "mpdm/scheduler.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>

namespace fs = std::filesystem;
using nlohmann::json;

namespace mpdm {

static const char* kActions[] = {"pause_all", "resume_all", "set_global_rate"};

static std::string gen_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    static const char* hex = "0123456789abcdef";
    std::string s(12, '0');
    for (char& c : s) c = hex[rng() & 0xF];
    return s;
}

static bool parse_hm(const std::string& at, int& h, int& m) {
    if (std::sscanf(at.c_str(), "%d:%d", &h, &m) != 2) return false;
    return h >= 0 && h < 24 && m >= 0 && m < 60;
}

static bool parse_once(const std::string& at, std::time_t& out) {
    std::string a = at;
    for (char& c : a) if (c == ' ') c = 'T';
    int Y, Mo, D, h, m, s = 0;
    int got = std::sscanf(a.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &Mo, &D, &h, &m, &s);
    if (got < 5) return false;
    std::tm t{};
    t.tm_year = Y - 1900;
    t.tm_mon = Mo - 1;
    t.tm_mday = D;
    t.tm_hour = h;
    t.tm_min = m;
    t.tm_sec = s;
    t.tm_isdst = -1;
    out = std::mktime(&t);
    return out != static_cast<std::time_t>(-1);
}

// Local-date midnight for a timestamp.
static std::time_t date_midnight(std::time_t t) {
    std::tm lt = *std::localtime(&t);
    lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
    lt.tm_isdst = -1;
    return std::mktime(&lt);
}

bool rule_fires_between(const ScheduleRule& r, std::time_t prev, std::time_t now) {
    if (!r.enabled || prev >= now) return false;
    if (r.repeat == "once") {
        std::time_t when;
        if (!parse_once(r.at, when)) return false;
        return prev < when && when <= now;
    }
    int h, m;
    if (!parse_hm(r.at, h, m)) return false;
    std::time_t end = date_midnight(now);
    for (std::time_t d = date_midnight(prev); d <= end;) {
        std::tm dt = *std::localtime(&d);
        dt.tm_hour = h;
        dt.tm_min = m;
        dt.tm_sec = 0;
        dt.tm_isdst = -1;
        std::time_t when = std::mktime(&dt);
        if (prev < when && when <= now) {
            if (r.days.empty()) return true;
            std::tm wt = *std::localtime(&when);
            int py_wday = (wt.tm_wday + 6) % 7;  // tm_wday 0=Sun -> python 0=Mon
            if (std::find(r.days.begin(), r.days.end(), py_wday) != r.days.end()) return true;
        }
        // advance one calendar day (re-normalize so DST can't drift us)
        std::time_t next = d + 24 * 3600;
        d = date_midnight(next);
    }
    return false;
}

// ---- Scheduler -------------------------------------------------------------

Scheduler::Scheduler(std::map<std::string, Action> actions, std::string store, double interval)
    : actions_(std::move(actions)), store_(std::move(store)), interval_(interval) {}

Scheduler::~Scheduler() { stop(); }

void Scheduler::load() {
    if (store_.empty()) return;
    std::error_code ec;
    if (!fs::exists(store_, ec)) return;
    try {
        std::ifstream f(store_, std::ios::binary);
        json root;
        f >> root;
        std::lock_guard<std::mutex> lk(mu_);
        rules_.clear();
        for (const auto& d : root.value("rules", json::array())) {
            ScheduleRule r;
            r.id = d.value("id", gen_id());
            r.action = d.value("action", "");
            r.at = d.value("at", "");
            r.repeat = d.value("repeat", "daily");
            if (d.contains("days") && d["days"].is_array())
                for (const auto& x : d["days"]) r.days.push_back(x.get<int>());
            r.rate_bps = d.value("rate_bps", 0LL);
            r.enabled = d.value("enabled", true);
            rules_.push_back(std::move(r));
        }
    } catch (const std::exception&) {
    }
}

void Scheduler::save() {
    if (store_.empty()) return;
    json arr = json::array();
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& r : rules_) {
            arr.push_back({{"id", r.id},
                           {"action", r.action},
                           {"at", r.at},
                           {"repeat", r.repeat},
                           {"days", r.days},
                           {"rate_bps", r.rate_bps},
                           {"enabled", r.enabled}});
        }
    }
    json root;
    root["rules"] = arr;
    try {
        std::string tmp = store_ + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f << root.dump(2);
        }
        fs::rename(tmp, store_);
    } catch (const std::exception&) {
    }
}

ScheduleRule Scheduler::add_rule(const std::string& action, const std::string& at,
                                 const std::string& repeat, std::vector<int> days,
                                 long long rate_bps, bool enabled) {
    if (std::find(std::begin(kActions), std::end(kActions), action) == std::end(kActions))
        throw std::invalid_argument("unknown schedule action: " + action);
    if (repeat != "daily" && repeat != "once")
        throw std::invalid_argument("unknown repeat: " + repeat);
    // Validate the time up front so a bad rule can't silently never fire.
    if (repeat == "once") {
        std::time_t tmp;
        if (!parse_once(at, tmp)) throw std::invalid_argument("invalid datetime: " + at);
    } else {
        int h, m;
        if (!parse_hm(at, h, m)) throw std::invalid_argument("invalid time of day: " + at);
    }
    ScheduleRule r;
    r.id = gen_id();
    r.action = action;
    r.at = at;
    r.repeat = repeat;
    r.days = std::move(days);
    r.rate_bps = rate_bps > 0 ? rate_bps : 0;
    r.enabled = enabled;
    {
        std::lock_guard<std::mutex> lk(mu_);
        rules_.push_back(r);
    }
    save();
    return r;
}

void Scheduler::remove_rule(const std::string& id) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        rules_.erase(std::remove_if(rules_.begin(), rules_.end(),
                                    [&](const ScheduleRule& r) { return r.id == id; }),
                     rules_.end());
    }
    save();
}

void Scheduler::set_enabled(const std::string& id, bool enabled) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& r : rules_)
            if (r.id == id) r.enabled = enabled;
    }
    save();
}

std::vector<ScheduleRule> Scheduler::list_rules() {
    std::lock_guard<std::mutex> lk(mu_);
    return rules_;
}

std::vector<ScheduleRule> Scheduler::due(std::time_t prev, std::time_t now) {
    std::vector<ScheduleRule> rules;
    {
        std::lock_guard<std::mutex> lk(mu_);
        rules = rules_;
    }
    std::vector<ScheduleRule> out;
    for (const auto& r : rules)
        if (rule_fires_between(r, prev, now)) out.push_back(r);
    return out;
}

void Scheduler::tick() { tick(std::time(nullptr)); }

void Scheduler::tick(std::time_t now) {
    std::optional<std::time_t> prev;
    {
        std::lock_guard<std::mutex> lk(mu_);
        prev = last_check_;
        last_check_ = now;
    }
    if (!prev) return;  // first tick establishes the baseline; no backfire
    for (const auto& r : due(*prev, now)) {
        auto it = actions_.find(r.action);
        if (it == actions_.end()) continue;
        try {
            it->second(r);
        } catch (...) {
        }
        if (r.repeat == "once") set_enabled(r.id, false);
    }
}

void Scheduler::start() {
    if (thread_.joinable()) return;
    stop_ = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        last_check_ = std::time(nullptr);
    }
    thread_ = std::thread([this] { loop(); });
}

void Scheduler::loop() {
    std::unique_lock<std::mutex> lk(cv_mu_);
    while (!stop_) {
        if (cv_.wait_for(lk, std::chrono::duration<double>(interval_),
                         [this] { return stop_.load(); }))
            break;
        lk.unlock();
        tick();
        lk.lock();
    }
}

void Scheduler::stop() {
    {
        std::lock_guard<std::mutex> lk(cv_mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

} // namespace mpdm
