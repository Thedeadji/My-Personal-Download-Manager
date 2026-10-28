// Multi-download queue — port of mpdm/manager.py (core: queue, concurrency,
// pause/resume/remove, persistence, global + per-item speed limits, incognito).
#pragma once
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "mpdm/config.hpp"
#include "mpdm/engine.hpp"
#include "mpdm/ratelimit.hpp"
#include "mpdm/scheduler.hpp"

namespace mpdm {

// Status strings (match the Python manager / downloads.json).
inline constexpr const char* ST_QUEUED = "queued";
inline constexpr const char* ST_DOWNLOADING = "downloading";
inline constexpr const char* ST_PAUSED = "paused";
inline constexpr const char* ST_COMPLETED = "completed";
inline constexpr const char* ST_ERROR = "error";

std::string state_dir();  // ~/.mypersonaldownloadmanager (created)

// Load the user's effective Settings: defaults -> settings.json -> environment.
// Used by standalone processes (the download popup) so they honor the same
// output folder / categories / limits the main app saved.
Settings load_user_settings();

struct DownloadItem;
// Browser downloads currently in progress, read from active/*.json (the popups
// are the sole writers). Returned as DownloadItem rows for the main app to show.
std::vector<DownloadItem> read_active_browser();

struct DownloadItem {
    std::string id;
    std::string url;
    std::string filename;
    std::string dest_path;
    std::optional<long long> size;
    long long downloaded = 0;
    double speed = 0.0;
    std::optional<double> eta;
    std::string status = ST_QUEUED;
    int connections = 0;          // 0 = use the manager default
    long long max_rate_bps = 0;   // per-download limit (0 = unlimited)
    bool incognito = false;
    std::map<std::string, std::string> headers;
    std::string error;
    double added_at = 0.0;
    std::vector<SegmentProgress> conn_stats;  // per-connection telemetry, runtime only

    std::shared_ptr<Download> dl;  // runtime only (not persisted)

    std::string category() const;
    std::optional<double> percent() const;
};

class DownloadManager {
public:
    explicit DownloadManager(Settings settings = {}, int max_concurrent = 3,
                             long long global_rate_bps = 0);
    ~DownloadManager();

    std::shared_ptr<DownloadItem> add(const std::string& url, const std::string& filename = "",
                                      int connections = 0,
                                      std::map<std::string, std::string> headers = {},
                                      bool start = true, long long max_rate_bps = 0,
                                      bool incognito = false);
    std::vector<std::shared_ptr<DownloadItem>> add_batch(const std::string& text,
                                                         int connections = 0, bool start = true,
                                                         long long max_rate_bps = 0,
                                                         bool incognito = false);

    void pump();
    void pause(const std::string& id);
    void resume(const std::string& id);
    void remove(const std::string& id, bool delete_file = false);
    void pause_all();
    void resume_all();
    void clear_finished();

    double global_rate_bps() { return global_limiter_.rate(); }
    void set_global_rate(double bps) { global_limiter_.set_rate(bps); }
    void set_item_rate(const std::string& id, long long bps);

    std::vector<std::shared_ptr<DownloadItem>> ordered();
    int active_count();

    void save();
    void load();
    void shutdown();  // cancel everything and join worker threads

    // Persist settings + manager-level prefs to settings.json.
    void save_settings();

    // Run a program / sleep / shut down when the whole queue finishes.
    void set_on_complete(const std::string& action, const std::string& command = "");

    // Scheduler (thin wrappers; the thread is not started until start_scheduler()).
    void start_scheduler();
    void stop_scheduler();
    ScheduleRule add_schedule(const std::string& action, const std::string& at,
                              const std::string& repeat = "daily", std::vector<int> days = {},
                              long long rate_bps = 0);
    void remove_schedule(const std::string& id);
    void set_schedule_enabled(const std::string& id, bool enabled);
    std::vector<ScheduleRule> list_schedules();

    // Incognito lifecycle: clear hard-kill leftovers / purge unfinished on close.
    void sweep_incognito();
    void purge_incognito_incomplete();

    // Browser integration: ingest completed-download records the popup dropped in
    // history_inbox/ into this manager (so they persist + show), once each.
    void ingest_history_inbox();

    Settings settings;
    int max_concurrent;
    std::string on_done_action = "none";  // none | run | sleep | shutdown
    std::string on_done_command;
    bool clip_monitor_pref = false;       // remembered UI pref (clipboard monitoring)

private:
    void run(std::shared_ptr<DownloadItem> it);
    void delete_files(const std::string& dest_path);
    bool queue_all_done();
    void maybe_on_complete();
    void run_completion_action(const std::string& action, const std::string& command);
    void wait_dl_release(const std::shared_ptr<DownloadItem>& it);

    std::map<std::string, std::shared_ptr<DownloadItem>> items_;
    std::vector<std::string> order_;
    std::recursive_mutex mu_;
    RateLimiter global_limiter_;
    std::string store_;
    std::vector<std::thread> threads_;
    std::atomic<bool> shutting_{false};
    std::atomic<bool> on_complete_fired_{false};
    std::unique_ptr<Scheduler> scheduler_;
};

} // namespace mpdm
