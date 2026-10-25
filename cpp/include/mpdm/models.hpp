// Core data models — port of mpdm/models.py.
#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace mpdm {

enum class Status { Pending, Probing, Downloading, Paused, Merging, Completed, Failed };

// One byte-range of the target file, fetched over its own connection.
struct Segment {
    int index = 0;
    long long start = 0;        // inclusive byte offset
    long long end = -1;         // inclusive byte offset
    long long downloaded = 0;   // bytes written so far

    long long size() const { return end - start + 1; }
    long long remaining() const { long long r = size() - downloaded; return r > 0 ? r : 0; }
    bool complete() const { return downloaded >= size(); }
    long long current_offset() const { return start + downloaded; }  // absolute resume offset
};

// What we learned about a URL before downloading.
struct RemoteInfo {
    std::string url;
    std::string final_url;
    std::optional<long long> size;          // Content-Length, or none if unknown
    bool accept_ranges = false;
    std::string filename;
    std::optional<std::string> etag;
    std::optional<std::string> last_modified;
    std::optional<std::string> content_type;
};

// Snapshot of progress, passed to UI callbacks.
struct Progress {
    std::optional<long long> total;
    long long downloaded = 0;
    double speed_bps = 0.0;
    Status status = Status::Pending;
    int segments = 0;
    int active_segments = 0;
    std::optional<double> eta_seconds;
    std::string filename;

    std::optional<double> percent() const {
        if (!total || *total == 0) return std::nullopt;
        double p = static_cast<double>(downloaded) * 100.0 / static_cast<double>(*total);
        return p < 100.0 ? p : 100.0;
    }
};

} // namespace mpdm
