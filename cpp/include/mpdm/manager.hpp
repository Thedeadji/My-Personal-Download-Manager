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
Settings load_user_settings();

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
    std::string output_dir;       // per-item save folder (empty = manager default)
    std::string proxy;            // per-item proxy (empty = manager default)
    std::map<std::string, std::string> headers;
    std::string error;
    double added_at = 0.0;
    std::vector<SegmentProgress> conn_stats;  // per-connection telemetry, runtime only

    bool stream = false;          // HLS/DASH grab (muxed to MP4) rather than a plain file
    int seg_done = 0, seg_total = 0;  // stream segment progress (runtime)

    // Automatic retry of a failed download (see Settings::auto_retry_*).
    double failed_since = 0.0;    // first failure of the current streak (epoch s; 0 = none)
    double failed_at = 0.0;       // most recent failure
    bool no_auto_retry = false;   // stopped on purpose (kept partial, bad checksum)

    // Checksum to verify the finished file against ("sha256:<hex>"; "" = none).
    std::string expected_hash;
    std::string hash_state;       // "" | "checking" | "ok" | "mismatch"

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
                                      bool incognito = false, bool stream = false);
    std::vector<std::shared_ptr<DownloadItem>> add_batch(const std::string& text,
                                                         int connections = 0, bool start = true,
                                                         long long max_rate_bps = 0,
                                                         bool incognito = false);

    void pump();
    void pause(const std::string& id);
    void resume(const std::string& id);
    // Point a stopped (paused/failed) download at a fresh link, e.g. when the old
    // one expired. Keeps the partial file: the engine resumes from it if the server
    // still reports the same file (size/ETag), otherwise it starts over. Returns
    // false if the item is unknown, running, or already complete.
    bool set_url(const std::string& id, const std::string& url);

    // Requeue failed downloads whose auto-retry wait is over. Call periodically
    // (the GUI does it every second). `now` is for tests; 0 = the current time.
    int retry_failed_due(double now = 0);
    // Seconds until this failed item is retried automatically; -1 = it won't be.
    double auto_retry_in(const DownloadItem& it, double now = 0);
    // Continue the downloads that were running/queued when the list was last saved
    // (Settings::resume_on_start). Call once the UI's resolvers are set up.
    int resume_interrupted();

    // Expected checksum (text as pasted: "<hex>", "sha256:<hex>", a SHA256SUMS
    // line…; "" clears it). A finished file is checked right away, an unfinished
    // one when it completes. False if the text isn't a checksum.
    bool set_expected_hash(const std::string& id, const std::string& text);
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

    // Resolver for the "ask" duplicate policy (a UI prompt). Each new download is
    // given this; "" if unset -> the engine falls back to "rename".
    void set_duplicate_resolver(DuplicateResolver r) { duplicate_resolver_ = std::move(r); }
    // Asked when a download's saved progress doesn't match the server's file any
    // more (continue / restart / keep). Unset -> the engine restarts.
    void set_resume_resolver(ResumeResolver r) { resume_resolver_ = std::move(r); }

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

    // Browser integration: adopt jobs the native host dropped in jobs/ (each a
    // browser download request) into the queue. Returns the new item ids.
    std::vector<std::string> ingest_jobs();

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
    void mark_failed(DownloadItem& it, const std::string& msg, bool no_auto_retry = false);
    bool verify_hash(DownloadItem& it);  // true = matches (or nothing to check)

    std::map<std::string, std::shared_ptr<DownloadItem>> items_;
    std::vector<std::string> order_;
    std::recursive_mutex mu_;
    DuplicateResolver duplicate_resolver_;
    ResumeResolver resume_resolver_;
    std::vector<std::string> interrupted_;  // ids that were mid-flight at load()
    RateLimiter global_limiter_;
    std::string store_;
    std::vector<std::thread> threads_;
    std::atomic<bool> shutting_{false};
    std::atomic<bool> on_complete_fired_{false};
    std::unique_ptr<Scheduler> scheduler_;
};

} // namespace mpdm
