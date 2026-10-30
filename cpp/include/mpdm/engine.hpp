// Segmented download engine — port of mpdm/engine.py (work-stealing + dynamic
// segmentation, resume, single-stream fallback, speed limiting).
#pragma once
#include <atomic>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>

#include "mpdm/config.hpp"
#include "mpdm/http_client.hpp"
#include "mpdm/models.hpp"
#include "mpdm/ratelimit.hpp"

namespace mpdm {

struct DownloadError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

using ProgressCb = std::function<void(const Progress&)>;
// Decide what to do about an already-existing file (the "ask" duplicate policy):
// return "rename", "skip" or "overwrite". Called from the download's worker thread.
using DuplicateResolver = std::function<std::string(const std::string& existing_path)>;

class Download {
public:
    Download(std::string url, Settings settings, std::string dest_path = "",
             ProgressCb cb = nullptr, std::map<std::string, std::string> headers = {},
             RateLimiter* global_limiter = nullptr);

    void pause() { pause_ = true; }
    void cancel() { cancel_ = true; }
    void set_rate(double bps) { limiter_.set_rate(bps); }
    void set_duplicate_resolver(DuplicateResolver r) { resolver_ = std::move(r); }
    // A caller-chosen filename (e.g. from the browser) used when no dest path is
    // given, so the engine still applies the duplicate-file policy to it instead of
    // the caller pre-resolving (and always renaming).
    void set_preferred_filename(std::string name) { preferred_filename_ = std::move(name); }
    long long total_downloaded();
    const std::string& dest_path() const { return dest_path_; }

    std::string start();  // run to completion; returns dest path; throws DownloadError

private:
    HttpOptions http_opts() const;
    void preallocate(long long size);
    void save();
    void emit_progress(bool force = false);
    void throttle(long long n);
    void interruptible_sleep(double seconds);
    Segment* claim_or_split();
    void download_segment(Segment* seg);
    std::string download_single();
    void finish_common();
    void relocate_from_temp();  // move a finished temp-dir file to its real destination
    void apply_file_time();     // stamp the finished file with the server's Last-Modified

    std::string url_;
    Settings settings_;
    std::string dest_path_;
    std::string final_path_;        // when a temp dir is used: the real destination
    std::string preferred_filename_;  // caller-chosen name (dup policy still applies)
    ProgressCb cb_;
    DuplicateResolver resolver_;
    std::map<std::string, std::string> headers_;
    RateLimiter limiter_;
    RateLimiter* global_limiter_;

    std::optional<RemoteInfo> info_;
    std::deque<Segment> segments_;   // stable addresses (push_back doesn't move)
    Status status_ = Status::Pending;

    std::mutex mu_;        // guards segment state, claimed_, active_, counters
    std::mutex file_mu_;   // guards positioned writes to file_
    std::fstream file_;
    std::atomic<bool> pause_{false};
    std::atomic<bool> cancel_{false};
    long long downloaded_at_start_ = 0;
    double t0_ = 0.0;
    double last_emit_ = 0.0;
    int active_ = 0;
    std::set<int> claimed_;
    int seg_counter_ = -1;
    std::map<int, double> seg_speed_;                      // index -> smoothed bytes/sec
    std::map<int, std::pair<long long, double>> seg_sample_;  // index -> (bytes, time) last sample
    std::map<int, int> seg_stole_;                        // index -> victim index it split from
};

} // namespace mpdm
