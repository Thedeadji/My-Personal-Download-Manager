#include "mpdm/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <locale>
#include <sstream>
#include <thread>
#include <vector>

#include "mpdm/categories.hpp"
#include "mpdm/gdrive.hpp"
#include "mpdm/segments.hpp"
#include "mpdm/storage.hpp"

namespace fs = std::filesystem;

namespace mpdm {

static double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

Download::Download(std::string url, Settings settings, std::string dest_path, ProgressCb cb,
                   std::map<std::string, std::string> headers, RateLimiter* global_limiter)
    : url_(std::move(url)),
      settings_(std::move(settings)),
      dest_path_(std::move(dest_path)),
      cb_(std::move(cb)),
      headers_(std::move(headers)),
      limiter_(static_cast<double>(settings_.max_rate_bps)),
      global_limiter_(global_limiter) {}

HttpOptions Download::http_opts() const {
    HttpOptions o;
    o.user_agent = settings_.user_agent;
    o.proxy = settings_.proxy;
    o.connect_timeout = settings_.connect_timeout;
    o.low_speed_time = settings_.read_timeout;
    o.low_speed_limit = 1;
    o.verify_tls = settings_.verify_tls;
    o.headers = headers_;
    return o;
}

long long Download::total_downloaded() {
    std::lock_guard<std::mutex> lk(mu_);
    long long t = 0;
    for (const auto& s : segments_) t += s.downloaded;
    return t;
}

void Download::throttle(long long n) {
    limiter_.consume(n);
    if (global_limiter_) global_limiter_->consume(n);
}

void Download::interruptible_sleep(double seconds) {
    double end = now_seconds() + seconds;
    while (now_seconds() < end) {
        if (cancel_ || pause_) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void Download::preallocate(long long size) {
    { std::ofstream f(dest_path_, std::ios::binary | std::ios::trunc); }
    std::error_code ec;
    if (size > 0) fs::resize_file(dest_path_, static_cast<std::uintmax_t>(size), ec);
}

void Download::save() {
    TaskRecord rec;
    rec.url = url_;
    rec.dest_path = dest_path_;
    rec.size = info_ ? info_->size : std::nullopt;
    rec.accept_ranges = info_ ? info_->accept_ranges : false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        rec.segments.assign(segments_.begin(), segments_.end());
    }
    rec.status = [this] {
        switch (status_) {
            case Status::Downloading: return "downloading";
            case Status::Paused: return "paused";
            case Status::Completed: return "completed";
            case Status::Failed: return "failed";
            default: return "pending";
        }
    }();
    rec.etag = info_ ? info_->etag : std::nullopt;
    rec.last_modified = info_ ? info_->last_modified : std::nullopt;
    save_record(dest_path_, rec);
}

void Download::emit_progress(bool force) {
    if (!cb_) return;
    double now = now_seconds();
    if (!force && (now - last_emit_) < 0.2) return;
    last_emit_ = now;
    long long done = total_downloaded();
    double elapsed = std::max(1e-6, now - t0_);
    double speed = static_cast<double>(done - downloaded_at_start_) / elapsed;
    std::optional<long long> total = info_ ? info_->size : std::nullopt;
    std::optional<double> eta;
    if (total && speed > 0) eta = std::max(0.0, static_cast<double>(*total - done) / speed);
    int nseg, act;
    std::vector<SegmentProgress> conns;
    {
        std::lock_guard<std::mutex> lk(mu_);
        nseg = static_cast<int>(segments_.size());
        act = active_;
        conns.reserve(segments_.size());
        for (const auto& s : segments_) {
            bool active = claimed_.count(s.index) > 0 && !s.complete();
            auto& smp = seg_sample_[s.index];  // (bytes, time)
            if (smp.second <= 0.0) {
                smp = {s.downloaded, now};
            } else if (now - smp.second >= 0.15) {
                double inst = static_cast<double>(s.downloaded - smp.first) / (now - smp.second);
                if (inst < 0) inst = 0;
                auto it = seg_speed_.find(s.index);
                seg_speed_[s.index] = it == seg_speed_.end() ? inst : it->second * 0.5 + inst * 0.5;
                smp = {s.downloaded, now};
            }
            SegmentProgress sp;
            sp.index = s.index;
            sp.start = s.start;
            sp.end = s.end;
            sp.downloaded = s.downloaded;
            sp.active = active;
            sp.speed_bps = active && seg_speed_.count(s.index) ? seg_speed_[s.index] : 0.0;
            auto st = seg_stole_.find(s.index);
            sp.stole_from = st == seg_stole_.end() ? -1 : st->second;
            conns.push_back(sp);
        }
    }
    Progress p;
    p.total = total;
    p.downloaded = done;
    p.speed_bps = speed;
    p.status = status_;
    p.segments = nseg;
    p.active_segments = act;
    p.eta_seconds = eta;
    p.filename = fs::path(dest_path_).filename().string();
    p.connections = std::move(conns);
    cb_(p);
}

Segment* Download::claim_or_split() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& s : segments_) {
        if (claimed_.count(s.index) == 0 && !s.complete()) {
            claimed_.insert(s.index);
            return &s;
        }
    }
    if (!settings_.dynamic_segmentation) return nullptr;
    Segment* victim = nullptr;
    long long best = -1;
    for (auto& s : segments_) {
        if (!s.complete() && s.remaining() > best) { best = s.remaining(); victim = &s; }
    }
    if (!victim || victim->remaining() < 2 * settings_.min_segment_size) return nullptr;
    long long cur = victim->current_offset();
    long long rem = victim->end - cur + 1;
    long long new_start = cur + rem / 2;
    long long old_end = victim->end;
    victim->end = new_start - 1;  // shrink victim
    int victim_index = victim->index;
    ++seg_counter_;
    segments_.push_back(Segment{seg_counter_, new_start, old_end, 0});
    claimed_.insert(seg_counter_);
    seg_stole_[seg_counter_] = victim_index;  // this connection took over part of victim's range
    return &segments_.back();
}

void Download::download_segment(Segment* seg) {
    if (seg->complete()) return;
    { std::lock_guard<std::mutex> lk(mu_); ++active_; }
    int attempt = 0;
    while (!seg->complete()) {
        if (cancel_ || pause_) break;
        long long start;
        long long end;
        { std::lock_guard<std::mutex> lk(mu_); start = seg->current_offset(); end = seg->end; }

        auto sink = [&](const char* data, size_t len) -> bool {
            if (cancel_ || pause_) return false;
            long long offset, remaining;
            { std::lock_guard<std::mutex> lk(mu_); offset = seg->current_offset();
              remaining = seg->end - offset + 1; }
            if (remaining <= 0) return false;  // shrunk by a split -> done
            size_t towrite = std::min<size_t>(len, static_cast<size_t>(remaining));
            { std::lock_guard<std::mutex> lk(file_mu_);
              file_.seekp(static_cast<std::streamoff>(offset));
              file_.write(data, static_cast<std::streamsize>(towrite)); }
            bool done;
            { std::lock_guard<std::mutex> lk(mu_); seg->downloaded += static_cast<long long>(towrite);
              done = seg->complete(); }
            throttle(static_cast<long long>(towrite));
            emit_progress();
            return !done;  // false stops the transfer once this segment is done
        };

        try {
            get_range(url_, start, end, http_opts(), sink);
            { std::lock_guard<std::mutex> lk(file_mu_); file_.flush(); }
            if (seg->complete() || cancel_ || pause_) break;
            // stream ended early -> loop re-requests the remainder
        } catch (const HttpError& e) {
            if (++attempt > settings_.max_retries) {
                { std::lock_guard<std::mutex> lk(mu_); --active_; }
                throw DownloadError(std::string("segment ") + std::to_string(seg->index) +
                                    " failed: " + e.what());
            }
            interruptible_sleep(settings_.retry_interval > 0 ? (double)settings_.retry_interval
                                                             : std::pow(settings_.retry_backoff, attempt));
        }
    }
    { std::lock_guard<std::mutex> lk(mu_); --active_; }
}

std::string Download::download_single() {
    status_ = Status::Downloading;
    long long known = (info_ && info_->size) ? *info_->size : (1LL << 62);
    { std::lock_guard<std::mutex> lk(mu_); segments_.clear();
      segments_.push_back(Segment{0, 0, known - 1, 0}); }
    long long resume_from = 0;
    std::error_code ec;
    if (fs::exists(dest_path_, ec) && info_ && info_->accept_ranges)
        resume_from = static_cast<long long>(fs::file_size(dest_path_, ec));

    file_.open(dest_path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!file_) file_.open(dest_path_, std::ios::binary | std::ios::out);
    file_.seekp(static_cast<std::streamoff>(resume_from));
    { std::lock_guard<std::mutex> lk(mu_); segments_[0].downloaded = resume_from; }

    int attempt = 0;
    while (true) {
        if (cancel_ || pause_) break;
        auto sink = [&](const char* data, size_t len) -> bool {
            if (cancel_ || pause_) return false;
            { std::lock_guard<std::mutex> lk(file_mu_); file_.write(data, (std::streamsize)len); }
            { std::lock_guard<std::mutex> lk(mu_); segments_[0].downloaded += (long long)len; }
            throttle((long long)len);
            emit_progress();
            return true;
        };
        try {
            long long from;
            { std::lock_guard<std::mutex> lk(mu_); from = segments_[0].downloaded; }
            get_range(url_, from, -1, http_opts(), sink);
            { std::lock_guard<std::mutex> lk(file_mu_); file_.flush(); }
            break;  // stream finished
        } catch (const HttpError& e) {
            if (++attempt > settings_.max_retries)
                throw DownloadError(std::string("single-stream download failed: ") + e.what());
            interruptible_sleep(settings_.retry_interval > 0 ? (double)settings_.retry_interval
                                                             : std::pow(settings_.retry_backoff, attempt));
        }
    }
    file_.close();
    if (cancel_) { status_ = Status::Failed; emit_progress(true); throw DownloadError("cancelled"); }
    if (pause_) { status_ = Status::Paused; save(); emit_progress(true); throw DownloadError("paused"); }
    status_ = Status::Completed;
    clear_record(dest_path_);
    relocate_from_temp();  // temp -> real destination (updates dest_path_)
    apply_file_time();
    emit_progress(true);
    return dest_path_;
}

// Move a finished temp-dir download to its real destination (updates dest_path_).
// Uses rename, falling back to copy+remove across drives; applies the duplicate
// policy if a real file appeared at the destination meanwhile.
void Download::relocate_from_temp() {
    if (final_path_.empty()) return;
    std::error_code ec;
    std::string dir = fs::path(final_path_).parent_path().string();
    fs::create_directories(dir, ec);
    std::string target = final_path_;
    if (fs::exists(target, ec) && settings_.duplicate_policy != "overwrite")
        target = unique_dest(dir, fs::path(final_path_).filename().string());
    fs::rename(dest_path_, target, ec);
    if (ec) {  // different drive: copy then remove the temp file
        ec.clear();
        fs::copy_file(dest_path_, target, fs::copy_options::overwrite_existing, ec);
        if (!ec) { std::error_code e2; fs::remove(dest_path_, e2); }
    }
    if (!ec) dest_path_ = target;
    final_path_.clear();
}

// Stamp the finished file with the server's Last-Modified date, if enabled.
void Download::apply_file_time() {
    if (!settings_.set_file_times || !info_ || !info_->last_modified) return;
    std::tm tm{};
    std::istringstream ss(*info_->last_modified);
    ss.imbue(std::locale::classic());
    ss >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S");  // RFC 1123 (e.g. "Wed, 21 Oct 2015 07:28:00 GMT")
    if (ss.fail()) return;
#ifdef _WIN32
    std::time_t t = _mkgmtime(&tm);
#else
    std::time_t t = timegm(&tm);
#endif
    if (t == static_cast<std::time_t>(-1)) return;
    try {
        auto sctp = std::chrono::system_clock::from_time_t(t);
        auto ftt = fs::file_time_type::clock::now() +
                   std::chrono::duration_cast<fs::file_time_type::duration>(
                       sctp - std::chrono::system_clock::now());
        std::error_code ec;
        fs::last_write_time(dest_path_, ftt, ec);
    } catch (...) {
    }
}

void Download::finish_common() {
    status_ = Status::Completed;
    if (info_ && info_->size) {
        std::error_code ec;
        long long actual = static_cast<long long>(fs::file_size(dest_path_, ec));
        if (!ec && actual != *info_->size) {
            status_ = Status::Failed;
            throw DownloadError("size mismatch: expected " + std::to_string(*info_->size) +
                                ", got " + std::to_string(actual));
        }
    }
    clear_record(dest_path_);
    relocate_from_temp();  // temp -> real destination (updates dest_path_)
    apply_file_time();
    emit_progress(true);
}

std::string Download::start() {
    t0_ = now_seconds();

    // 0. Google Drive: rewrite share/interstitial URLs to the real file URL
    //    (handles the "virus scan" page + confirm token) before probing.
    bool drive = is_drive_url(url_);
    std::optional<std::string> drive_name;
    if (drive) {
        GDriveResult r = resolve_drive(url_, http_opts());  // never throws
        url_ = r.url;
        drive_name = r.filename;
    }

    status_ = Status::Probing;
    emit_progress(true);
    info_ = probe(url_, http_opts());

    // Guard: don't silently save Drive's HTML confirmation/sign-in page.
    if (drive && info_->content_type) {
        std::string ct = *info_->content_type;
        std::transform(ct.begin(), ct.end(), ct.begin(), ::tolower);
        if (ct.rfind("text/html", 0) == 0) {
            status_ = Status::Failed;
            emit_progress(true);
            throw DownloadError(
                "Google Drive returned its confirmation/sign-in page, not the file. The file may "
                "be private (download via the browser extension while signed in) or require manual "
                "confirmation.");
        }
    }

    // Drive URLs don't carry the real filename, so a plain save becomes "download".
    // Use the name we resolved from Content-Disposition / the page.
    if (drive && drive_name && !drive_name->empty()) {
        std::string good = sanitize_filename(*drive_name);
        if (!dest_path_.empty()) {
            std::string base = fs::path(dest_path_).filename().string();
            std::string blow = base;
            std::transform(blow.begin(), blow.end(), blow.begin(), ::tolower);
            if (blow == "download" || base.empty() || base.find('.') == std::string::npos) {
                std::string out_dir = categorized_dir(settings_.output_dir, good,
                                                      settings_.categorize, settings_.category_dirs,
                                                      settings_.category_exts);
                std::error_code ec;
                fs::create_directories(out_dir, ec);
                dest_path_ = unique_dest(out_dir, good);
            }
        } else {
            info_->filename = good;
        }
    }

    if (dest_path_.empty()) {
        // Honor a caller-chosen filename (browser / Add box) but still run the
        // duplicate-file policy on it below.
        if (!preferred_filename_.empty()) info_->filename = preferred_filename_;
        std::string out_dir = categorized_dir(settings_.output_dir, info_->filename,
                                              settings_.categorize, settings_.category_dirs,
                                              settings_.category_exts);
        std::error_code ec;
        fs::create_directories(out_dir, ec);
        auto resumable = find_resumable(out_dir, url_, info_->size, info_->etag);
        if (resumable) {
            dest_path_ = *resumable;  // a matching partial always wins (true resume)
        } else {
            // Duplicate-file policy: a full file with this name already on disk.
            std::string candidate =
                (fs::path(out_dir) / sanitize_filename(info_->filename)).string();
            std::string pol = settings_.duplicate_policy;
            if (pol == "ask" && fs::exists(candidate, ec)) {
                pol = resolver_ ? resolver_(candidate) : "rename";
                if (pol != "skip" && pol != "overwrite" && pol != "rename") pol = "rename";
            }
            if (fs::exists(candidate, ec) && pol == "skip") {
                // Keep the existing file; report it complete without re-downloading.
                dest_path_ = candidate;
                long long fsz = static_cast<long long>(fs::file_size(candidate, ec));
                if (!info_->size) info_->size = fsz;
                Segment seg;
                seg.index = 0;
                seg.start = 0;
                seg.end = (info_->size ? *info_->size : fsz) - 1;
                seg.downloaded = seg.size();
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    segments_ = {seg};
                }
                downloaded_at_start_ = seg.downloaded;
                status_ = Status::Completed;
                emit_progress(true);
                clear_record(dest_path_);
                return dest_path_;
            } else if (fs::exists(candidate, ec) && pol == "overwrite") {
                dest_path_ = candidate;  // preallocate() truncates it below
            } else {
                dest_path_ = unique_dest(out_dir, info_->filename);  // "rename" (default)
            }
        }
    }

    // Temporary/work directory: download (and checkpoint) here, then move the
    // finished file to its real destination. Resume still works — an interrupted
    // temp file keeps its sidecar, and the final name resolves to the same temp
    // path next time (the final file doesn't exist yet).
    if (!settings_.temp_dir.empty()) {
        std::error_code ec;
        fs::create_directories(settings_.temp_dir, ec);
        if (fs::is_directory(settings_.temp_dir, ec)) {
            final_path_ = dest_path_;
            dest_path_ = (fs::path(settings_.temp_dir) / fs::path(dest_path_).filename()).string();
        }
    }

    bool resumed = false;
    if (auto rec = load_record(dest_path_)) {
        std::error_code ec;
        bool mtime_ok = settings_.resume_ignore_mtime || rec->last_modified == info_->last_modified;
        if (rec->size == info_->size && rec->etag == info_->etag && mtime_ok && fs::exists(dest_path_, ec)) {
            segments_.assign(rec->segments.begin(), rec->segments.end());
            long long s = 0;
            for (const auto& seg : segments_) s += seg.downloaded;
            downloaded_at_start_ = s;
            resumed = !segments_.empty();
        }
    }

    if (!resumed) {
        if (info_->accept_ranges && info_->size && *info_->size > 0) {
            auto segs = plan_segments(*info_->size, settings_.connections, settings_.min_segment_size);
            segments_.assign(segs.begin(), segs.end());
            preallocate(*info_->size);
        } else {
            return download_single();
        }
    }

    status_ = Status::Downloading;
    emit_progress(true);
    save();

    file_.open(dest_path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!file_) throw DownloadError("cannot open output file: " + dest_path_);

    std::atomic<bool> stop_ckpt{false};
    std::thread ckpt([&] {
        while (!stop_ckpt) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!stop_ckpt) save();
        }
    });

    claimed_.clear();
    seg_counter_ = -1;
    int incomplete = 0;
    for (const auto& s : segments_) { seg_counter_ = std::max(seg_counter_, s.index); if (!s.complete()) ++incomplete; }
    int nworkers = std::max(1, std::min(settings_.connections, incomplete));

    std::vector<std::string> errors;
    std::mutex err_mu;
    auto worker = [&] {
        while (!cancel_ && !pause_) {
            Segment* seg = claim_or_split();
            if (!seg) return;
            try {
                download_segment(seg);
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lk(err_mu);
                errors.emplace_back(e.what());
                return;
            }
        }
    };
    std::vector<std::thread> pool;
    for (int i = 0; i < nworkers; ++i) pool.emplace_back(worker);
    for (auto& t : pool) t.join();

    stop_ckpt = true;
    ckpt.join();
    { std::lock_guard<std::mutex> lk(file_mu_); if (file_.is_open()) file_.close(); }
    save();

    if (cancel_) { status_ = Status::Failed; emit_progress(true); throw DownloadError("cancelled"); }
    if (pause_) { status_ = Status::Paused; save(); emit_progress(true); throw DownloadError("paused"); }
    if (!errors.empty()) { status_ = Status::Failed; save(); emit_progress(true); throw DownloadError(errors.front()); }

    bool all_done = true;
    for (const auto& s : segments_) if (!s.complete()) { all_done = false; break; }
    if (!all_done) { status_ = Status::Failed; save(); throw DownloadError("incomplete after workers finished"); }

    finish_common();
    return dest_path_;
}

} // namespace mpdm
