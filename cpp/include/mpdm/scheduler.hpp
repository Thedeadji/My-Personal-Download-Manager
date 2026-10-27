// Time-based scheduling of queue actions — port of mpdm/scheduler.py.
// A background thread edge-fires rules whose scheduled time the clock has crossed
// since the last tick (each occurrence fires once; never backfires).
#pragma once
#include <atomic>
#include <condition_variable>
#include <ctime>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace mpdm {

struct ScheduleRule {
    std::string id;
    std::string action;            // "pause_all" | "resume_all" | "set_global_rate"
    std::string at;                // "HH:MM" (daily) or ISO "YYYY-MM-DDTHH:MM" (once)
    std::string repeat = "daily";  // "daily" | "once"
    std::vector<int> days;         // weekday filter 0=Mon..6=Sun; empty = every day
    long long rate_bps = 0;        // payload for set_global_rate
    bool enabled = true;
};

// Did this rule's scheduled time fall in the interval (prev, now]? (pure/testable)
bool rule_fires_between(const ScheduleRule& rule, std::time_t prev, std::time_t now);

class Scheduler {
public:
    using Action = std::function<void(const ScheduleRule&)>;

    explicit Scheduler(std::map<std::string, Action> actions, std::string store = "",
                       double interval = 15.0);
    ~Scheduler();

    void load();
    void save();

    ScheduleRule add_rule(const std::string& action, const std::string& at,
                          const std::string& repeat = "daily", std::vector<int> days = {},
                          long long rate_bps = 0, bool enabled = true);
    void remove_rule(const std::string& id);
    void set_enabled(const std::string& id, bool enabled);
    std::vector<ScheduleRule> list_rules();

    std::vector<ScheduleRule> due(std::time_t prev, std::time_t now);
    void tick();                 // fire using the current time
    void tick(std::time_t now);  // fire using an injected time (tests)

    void start();
    void stop();

private:
    void loop();

    std::map<std::string, Action> actions_;
    std::vector<ScheduleRule> rules_;
    std::string store_;
    double interval_;
    std::mutex mu_;                       // guards rules_ + last_check_
    std::optional<std::time_t> last_check_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::condition_variable cv_;
    std::mutex cv_mu_;
};

} // namespace mpdm
