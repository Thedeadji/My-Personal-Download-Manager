// HLS (.m3u8) parsing — port of mpdm/hls.py (master+media, #EXT-X-MAP,
// per-segment #EXT-X-KEY + IV, AES-128 key tracking with rotation).
#pragma once
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "mpdm/http_client.hpp"

namespace mpdm {

struct Variant {
    std::string uri;
    int bandwidth = 0;
    std::string resolution;
    std::string codecs;
};

struct SegmentKey {
    std::string method = "NONE";          // "NONE", "AES-128", "SAMPLE-AES", …
    std::optional<std::string> uri;       // absolute key URI
    std::optional<std::string> iv;        // explicit 16-byte IV (raw), or none
};

struct MediaSegment {
    std::string uri;
    long long seq = 0;                     // media sequence number (for default IV)
    std::optional<SegmentKey> key;         // none / method "NONE" => unencrypted
};

struct MediaPlaylist {
    std::vector<std::string> segments;     // URIs (back-compat)
    std::vector<MediaSegment> seg_infos;
    std::optional<std::string> init_segment;
    std::optional<std::string> key_uri;    // first key seen
    std::string key_method = "NONE";
    long long media_sequence = 0;
    double target_duration = 0.0;
    bool is_endlist = false;
    bool is_encrypted() const;
};

bool is_master(const std::string& text);
std::vector<Variant> parse_master(const std::string& text, const std::string& base_url);
MediaPlaylist parse_media(const std::string& text, const std::string& base_url);
std::optional<std::string> parse_iv(const std::string& raw);  // "0x…" -> 16 raw bytes
std::string segment_iv(const MediaSegment& seg);              // explicit IV or seq (16 bytes)

// Network: fetch `url`; if a master playlist, pick a variant and fetch it.
std::pair<MediaPlaylist, std::string> resolve_media_playlist(const std::string& url,
                                                             const HttpOptions& opt,
                                                             const std::string& prefer = "best");
std::string fetch_key(const std::string& uri, const HttpOptions& opt);  // 16 bytes

} // namespace mpdm
