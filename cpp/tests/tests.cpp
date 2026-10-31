// Backend unit tests — validate the ported logic against the Python behavior.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32  // httplib pulls in windows.h: keep its macros (min/max, small) out
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include <httplib.h>
#ifdef small
#undef small
#endif
#include <nlohmann/json.hpp>

#include "mpdm/aes128.hpp"
#include "mpdm/batch.hpp"
#include "mpdm/categories.hpp"
#include "mpdm/checksum.hpp"
#include "mpdm/config.hpp"
#include "mpdm/dash.hpp"
#include "mpdm/gdrive.hpp"
#include "mpdm/grabber.hpp"
#include "mpdm/hls.hpp"
#include "mpdm/manager.hpp"
#include "mpdm/models.hpp"
#include "mpdm/native_host.hpp"
#include "mpdm/ratelimit.hpp"
#include "mpdm/scheduler.hpp"
#include "mpdm/segments.hpp"
#include "mpdm/storage.hpp"

static int g_failures = 0;
#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::cerr << "FAIL: " #cond "  (" << __FILE__ << ":" << __LINE__      \
                      << ")\n";                                                    \
            ++g_failures;                                                         \
        }                                                                         \
    } while (0)

using namespace mpdm;
namespace fs = std::filesystem;

static void test_plan_segments() {
    long long size = 10'000'000;
    auto segs = plan_segments(size, 8, 256 * 1024);
    CHECK(segs.front().start == 0);
    CHECK(segs.back().end == size - 1);
    long long total = 0;
    for (size_t i = 0; i < segs.size(); ++i) {
        total += segs[i].size();
        if (i) CHECK(segs[i].start == segs[i - 1].end + 1);  // contiguous
    }
    CHECK(total == size);

    auto small = plan_segments(1000, 8, 256 * 1024);
    CHECK(small.size() == 1);
    CHECK(small[0].size() == 1000);
}

static void test_parse_rate() {
    CHECK(parse_rate("500k") == 500 * 1024);
    CHECK(parse_rate("500kb") == 500 * 1024);
    CHECK(parse_rate("500KiB/s") == 500 * 1024);
    CHECK(parse_rate("1.5m") == (long long)(1.5 * 1024 * 1024));
    CHECK(parse_rate("2g") == 2LL * 1024 * 1024 * 1024);
    CHECK(parse_rate("1000000") == 1000000);
    CHECK(parse_rate("2mbps") == 2LL * 1024 * 1024);
    CHECK(parse_rate("") == 0);
    CHECK(parse_rate("unlimited") == 0);
    CHECK(parse_rate("0") == 0);
    bool threw = false;
    try { parse_rate("fast"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);

    CHECK(format_rate(0) == "unlimited");
    CHECK(format_rate(1024) == "1.0 KB/s");
    CHECK(format_rate(1024 * 1024) == "1.0 MB/s");
}

static void test_categories() {
    CHECK(classify("movie.mp4") == "Video");
    CHECK(classify("song.MP3") == "Audio");
    CHECK(classify("a.zip") == "Archives");
    CHECK(classify("setup.exe") == "Programs");
    CHECK(classify("x.xyz") == "Other");
    CHECK(classify("noext") == "Other");

    CHECK(category_subdir("a.mp3") == "Music");        // Audio -> Music
    CHECK(category_subdir("a.zip") == "Compressed");   // Archives -> Compressed
    CHECK(category_subdir("a.xyz").empty());

    std::string base = (fs::path("C:") / "dl").string();
    CHECK(fs::path(categorized_dir(base, "a.mp4")) == fs::path(base) / "Video");
    CHECK(fs::path(categorized_dir(base, "a.mp3")) == fs::path(base) / "Music");
    CHECK(fs::path(categorized_dir(base, "a.xyz")) == fs::path(base));           // uncategorized
    CHECK(fs::path(categorized_dir(base, "a.mp4", false)) == fs::path(base));    // disabled

    // An absolute override path (D:\Movies on Windows, /media/Movies elsewhere).
#ifdef _WIN32
    const fs::path absMovies = fs::path("D:\\") / "Movies";
#else
    const fs::path absMovies = fs::path("/media") / "Movies";
#endif
    std::map<std::string, std::string> ov = {{"Video", absMovies.string()}, {"Audio", "tunes"}};
    CHECK(fs::path(categorized_dir(base, "a.mp4", true, ov)) == absMovies);  // abs
    CHECK(fs::path(categorized_dir(base, "a.mp3", true, ov)) == fs::path(base) / "tunes");   // rel
    CHECK(fs::path(categorized_dir(base, "a.zip", true, ov)) == fs::path(base) / "Compressed");
}

static void test_batch() {
    CHECK((expand_pattern("https://h/file.zip") == std::vector<std::string>{"https://h/file.zip"}));
    CHECK((expand_pattern("https://h/img[1-3].jpg") ==
           std::vector<std::string>{"https://h/img1.jpg", "https://h/img2.jpg", "https://h/img3.jpg"}));
    CHECK((expand_pattern("https://h/f[001-003].zip") ==
           std::vector<std::string>{"https://h/f001.zip", "https://h/f002.zip", "https://h/f003.zip"}));
    CHECK((expand_pattern("https://h/[3-1].ts") ==
           std::vector<std::string>{"https://h/3.ts", "https://h/2.ts", "https://h/1.ts"}));
    CHECK((expand_pattern("https://h/ch[a-c].ts") ==
           std::vector<std::string>{"https://h/cha.ts", "https://h/chb.ts", "https://h/chc.ts"}));
    CHECK((expand_pattern("https://h/[a-b]/[1-2].ts") ==
           std::vector<std::string>{"https://h/a/1.ts", "https://h/a/2.ts",
                                    "https://h/b/1.ts", "https://h/b/2.ts"}));
    auto urls = expand_urls("https://h/a.zip\nhttps://h/img[1-2].png  https://h/a.zip");
    CHECK((urls == std::vector<std::string>{"https://h/a.zip", "https://h/img1.png", "https://h/img2.png"}));
}

static void test_storage_roundtrip() {
    auto dir = fs::temp_directory_path() / fs::path("mpdm_test_" + std::to_string(::time(nullptr)));
    fs::create_directories(dir);
    std::string dest = (dir / "file.bin").string();

    CHECK(sidecar_path(dest) == dest + ".mpdm");

    TaskRecord rec;
    rec.url = "https://h/file.bin";
    rec.dest_path = dest;
    rec.size = 1000;
    rec.accept_ranges = true;
    rec.status = "downloading";
    rec.etag = "\"abc\"";
    rec.segments = {Segment{0, 0, 499, 100}, Segment{1, 500, 999, 0}};
    save_record(dest, rec);

    auto back = load_record(dest);
    CHECK(back.has_value());
    CHECK(back->url == rec.url);
    CHECK(back->size == rec.size);
    CHECK(back->etag == rec.etag);
    CHECK(back->segments.size() == 2);
    CHECK(back->segments[0].downloaded == 100);
    CHECK(back->segments[1].start == 500);

    // find_resumable matches on url+size+etag when the file exists.
    { std::ofstream f(dest, std::ios::binary); f << "x"; }
    auto found = find_resumable(dir.string(), rec.url, rec.size, rec.etag);
    CHECK(found.has_value() && *found == dest);
    CHECK(!find_resumable(dir.string(), rec.url, 9999, rec.etag).has_value());  // size mismatch

    clear_record(dest);
    CHECK(!load_record(dest).has_value());

    // unique_dest avoids clobbering.
    std::string u = unique_dest(dir.string(), "file.bin");  // file.bin exists -> "file (1).bin"
    CHECK(fs::path(u).filename().string() == "file (1).bin");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

static void set_home(const std::string& dir) {
#ifdef _WIN32
    _putenv_s("USERPROFILE", dir.c_str());
#else
    setenv("HOME", dir.c_str(), 1);
#endif
}

static void test_manager() {
    auto tmp = (fs::temp_directory_path() / fs::path("mpdm_mgr_" + std::to_string(::time(nullptr)))).string();
    fs::create_directories(tmp);
    set_home(tmp);

    mpdm::DownloadManager m(mpdm::Settings{}, 2, 0);
    auto a = m.add("http://127.0.0.1:1/a.zip", "", 0, {}, /*start=*/false);
    auto secret = m.add("http://127.0.0.1:1/secret.zip", "", 0, {}, false, 0, /*incognito=*/true);
    CHECK(secret->incognito);
    CHECK(m.ordered().size() == 2);
    CHECK(m.ordered()[0]->url == a->url);  // insertion order preserved

    // Persistence: incognito excluded; normal kept.
    std::string store = (fs::path(mpdm::state_dir()) / "downloads.json").string();
    nlohmann::json root;
    { std::ifstream f(store); f >> root; }
    std::vector<std::string> urls;
    for (auto& j : root["items"]) urls.push_back(j["url"].get<std::string>());
    CHECK(std::find(urls.begin(), urls.end(), a->url) != urls.end());
    CHECK(std::find(urls.begin(), urls.end(), secret->url) == urls.end());

    // Per-item + global rate.
    m.set_item_rate(a->id, 500 * 1024);
    CHECK(a->max_rate_bps == 500 * 1024);
    m.set_global_rate(1024 * 1024);
    CHECK(m.global_rate_bps() == 1024 * 1024);

    // New link for a failed download: URL swapped, error cleared, persisted.
    // Refused for an unknown id, an empty URL, or a finished download.
    a->status = mpdm::ST_ERROR;
    a->error = "HTTP 403";
    CHECK(m.set_url(a->id, "http://127.0.0.1:1/a-fresh.zip"));
    CHECK(a->url == "http://127.0.0.1:1/a-fresh.zip");
    CHECK(a->error.empty());
    CHECK(!m.set_url("no-such-id", "http://127.0.0.1:1/x.zip"));
    CHECK(!m.set_url(a->id, ""));
    a->status = mpdm::ST_COMPLETED;
    CHECK(!m.set_url(a->id, "http://127.0.0.1:1/other.zip"));
    CHECK(a->url == "http://127.0.0.1:1/a-fresh.zip");
    a->status = mpdm::ST_PAUSED;
    m.save();

    // A fresh manager reloads the normal item (as paused), not the incognito one.
    mpdm::DownloadManager m2(mpdm::Settings{}, 2, 0);
    bool has_a = false, has_secret = false;
    for (auto& it : m2.ordered()) {
        if (it->url == a->url) { has_a = true; CHECK(it->status == mpdm::ST_PAUSED); }
        if (it->url == secret->url) has_secret = true;
    }
    CHECK(has_a);
    CHECK(!has_secret);

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

static std::string unhex(const std::string& h) {
    auto v = [](char c) -> int {
        c = (char)std::tolower((unsigned char)c);
        return c <= '9' ? c - '0' : c - 'a' + 10;
    };
    std::string o;
    for (size_t i = 0; i + 1 < h.size(); i += 2) o.push_back((char)((v(h[i]) << 4) | v(h[i + 1])));
    return o;
}

static void test_aes() {
    // NIST SP 800-38A F.2 CBC-AES128 (no PKCS#7 -> unpad=false).
    std::string key = unhex("2b7e151628aed2a6abf7158809cf4f3c");
    std::string iv = unhex("000102030405060708090a0b0c0d0e0f");
    std::string ct = unhex("7649abac8119b246cee98e9b12e9197d"
                           "5086cb9b507219ee95db113a917678b2"
                           "73bed6b8e3c1743b7116e69e22229516"
                           "3ff1caa1681fac09120eca307586e1a7");
    std::string pt = unhex("6bc1bee22e409f96e93d7e117393172a"
                           "ae2d8a571e03ac9c9eb76fac45af8e51"
                           "30c81c46a35ce411e5fbc1191a0a52ef"
                           "f69f2445df4f9b17ad2b417be66c3710");
    CHECK(aes128_cbc_decrypt(key, iv, ct, false) == pt);

    CHECK(pkcs7_unpad(std::string("hello") + std::string(11, (char)11)) == "hello");
    CHECK(pkcs7_unpad(std::string(16, 'A') + std::string(16, (char)16)) == std::string(16, 'A'));
    CHECK(pkcs7_unpad("abc\x05\x05") == "abc\x05\x05");  // invalid padding -> unchanged
}

static void test_hls() {
    const char* ENC =
        "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:10\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"https://host/key1.bin\"\n#EXTINF:6.0,\nseg10.ts\n"
        "#EXTINF:6.0,\nseg11.ts\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"https://host/key2.bin\",IV=0x00000000000000000000000000000063\n"
        "#EXTINF:6.0,\nseg12.ts\n#EXT-X-KEY:METHOD=NONE\n#EXTINF:6.0,\nseg13.ts\n#EXT-X-ENDLIST\n";
    auto pl = parse_media(ENC, "https://host/media.m3u8");
    CHECK(pl.media_sequence == 10);
    CHECK(pl.is_encrypted());
    CHECK(pl.seg_infos.size() == 4);
    CHECK(pl.seg_infos[0].uri == "https://host/seg10.ts");
    CHECK(pl.seg_infos[0].seq == 10 && pl.seg_infos[3].seq == 13);
    CHECK(pl.seg_infos[0].key && *pl.seg_infos[0].key->uri == "https://host/key1.bin");
    CHECK(pl.seg_infos[0].key && !pl.seg_infos[0].key->iv.has_value());
    CHECK(pl.seg_infos[2].key && *pl.seg_infos[2].key->uri == "https://host/key2.bin");
    std::string iv63(16, '\0'); iv63[15] = (char)0x63;
    CHECK(pl.seg_infos[2].key && pl.seg_infos[2].key->iv == iv63);
    CHECK(!pl.seg_infos[3].key.has_value());
    std::string iv10(16, '\0'); iv10[15] = (char)10;
    CHECK(segment_iv(pl.seg_infos[0]) == iv10);  // default IV from sequence number

    const char* MASTER =
        "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=1280x720\nhigh.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=200000,RESOLUTION=640x360\nlow.m3u8\n";
    CHECK(is_master(MASTER));
    auto vars = parse_master(MASTER, "https://h/x.m3u8");
    CHECK(vars.size() == 2 && vars[0].bandwidth == 800000);  // sorted desc
    CHECK(vars[0].uri == "https://h/high.m3u8");
}

static void test_dash() {
    const char* TPL =
        "<MPD mediaPresentationDuration=\"PT6S\"><Period><AdaptationSet mimeType=\"video/mp4\">"
        "<SegmentTemplate media=\"$RepresentationID$-$Number%03d$.m4s\" "
        "initialization=\"$RepresentationID$-init.mp4\" startNumber=\"1\">"
        "<SegmentTimeline><S t=\"0\" d=\"2\" r=\"2\"/></SegmentTimeline></SegmentTemplate>"
        "<Representation id=\"v0\" bandwidth=\"800000\"/>"
        "<Representation id=\"v1\" bandwidth=\"200000\"/></AdaptationSet></Period></MPD>";
    auto s = parse_manifest(TPL, "https://h/mpd/x.mpd", "best");
    CHECK(s.bandwidth == 800000);
    CHECK(s.init_url && *s.init_url == "https://h/mpd/v0-init.mp4");
    CHECK((s.segment_urls == std::vector<std::string>{
        "https://h/mpd/v0-001.m4s", "https://h/mpd/v0-002.m4s", "https://h/mpd/v0-003.m4s"}));

    const char* LIST =
        "<MPD><Period><BaseURL>https://h/v/</BaseURL><AdaptationSet mimeType=\"video/mp4\">"
        "<Representation id=\"r0\" bandwidth=\"500000\"><SegmentList duration=\"2\">"
        "<Initialization sourceURL=\"init.mp4\"/><SegmentURL media=\"seg1.m4s\"/>"
        "<SegmentURL media=\"seg2.m4s\"/></SegmentList></Representation></AdaptationSet></Period></MPD>";
    auto l = parse_manifest(LIST, "https://h/mpd/x.mpd");
    CHECK(l.init_url && *l.init_url == "https://h/v/init.mp4");
    CHECK((l.segment_urls == std::vector<std::string>{"https://h/v/seg1.m4s", "https://h/v/seg2.m4s"}));

    const char* BASE =
        "<MPD><Period><AdaptationSet mimeType=\"video/mp4\"><Representation id=\"r0\" bandwidth=\"900000\">"
        "<BaseURL>video-900k.mp4</BaseURL><SegmentBase indexRange=\"0-999\">"
        "<Initialization range=\"0-833\"/></SegmentBase></Representation></AdaptationSet></Period></MPD>";
    auto b = parse_manifest(BASE, "https://h/mpd/x.mpd");
    CHECK(!b.init_url.has_value());
    CHECK((b.segment_urls == std::vector<std::string>{"https://h/mpd/video-900k.mp4"}));
}

static void test_grabber_plan() {
    // Unencrypted media playlist -> empty plan (nothing to decrypt), no network.
    const char* PLAIN =
        "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXT-X-MEDIA-SEQUENCE:0\n"
        "#EXTINF:4.0,\nseg0.ts\n#EXTINF:4.0,\nseg1.ts\n#EXT-X-ENDLIST\n";
    auto plain = parse_media(PLAIN, "https://h/v/index.m3u8");
    CHECK(!plain.is_encrypted());
    CHECK(hls_decrypt_plan(plain, HttpOptions{}).empty());

    // SAMPLE-AES must be rejected (only AES-128 is supported) — throws before any
    // key fetch, so this stays network-free.
    const char* SAMPLE =
        "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXT-X-MEDIA-SEQUENCE:0\n"
        "#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"https://h/k.key\"\n"
        "#EXTINF:4.0,\nseg0.ts\n#EXT-X-ENDLIST\n";
    auto sample = parse_media(SAMPLE, "https://h/v/index.m3u8");
    CHECK(sample.is_encrypted());
    bool threw = false;
    try {
        hls_decrypt_plan(sample, HttpOptions{});
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

static void test_config_ua() {
    CHECK(resolve_user_agent("") == DEFAULT_USER_AGENT);
    CHECK(resolve_user_agent("default") == DEFAULT_USER_AGENT);
    CHECK(resolve_user_agent("honest") == DEFAULT_USER_AGENT);
    CHECK(resolve_user_agent("browser") == BROWSER_USER_AGENT);
    CHECK(resolve_user_agent("Chrome") == BROWSER_USER_AGENT);
    CHECK(resolve_user_agent("MyCustomAgent/1.0") == "MyCustomAgent/1.0");
}

static void test_clean() {
    fs::path dir = fs::temp_directory_path() / ("mpdm_clean_" + std::to_string(std::time(nullptr)));
    fs::create_directories(dir);

    // An incomplete download: a target file + a sidecar whose segment isn't done.
    std::string dest = (dir / "file.bin").string();
    { std::ofstream(dest, std::ios::binary) << "partial"; }
    TaskRecord rec;
    rec.url = "http://h/file.bin";
    rec.dest_path = dest;
    rec.size = 1000;
    Segment seg;
    seg.index = 0; seg.start = 0; seg.end = 999; seg.downloaded = 7;  // not complete
    rec.segments.push_back(seg);
    rec.status = "downloading";
    save_record(dest, rec);
    CHECK(fs::exists(sidecar_path(dest)));

    // A stray checkpoint file.
    std::string tmp = (dir / "stray.tmp").string();
    { std::ofstream(tmp, std::ios::binary) << "x"; }

    // Without --purge: sidecar + tmp go, the incomplete file stays.
    CleanResult r1 = clean_dir(dir.string(), /*purge*/ false);
    CHECK(r1.sidecars.size() == 1);
    CHECK(r1.tmp.size() == 1);
    CHECK(r1.partials.empty());
    CHECK(!fs::exists(sidecar_path(dest)));
    CHECK(fs::exists(dest));

    // Recreate the sidecar; with --purge the incomplete file goes too.
    save_record(dest, rec);
    CleanResult r2 = clean_dir(dir.string(), /*purge*/ true);
    CHECK(r2.sidecars.size() == 1);
    CHECK(r2.partials.size() == 1);
    CHECK(!fs::exists(dest));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

static void test_gdrive() {
    CHECK(is_drive_url("https://drive.google.com/file/d/ABC123defg_/view?usp=sharing"));
    CHECK(is_drive_url("https://drive.usercontent.google.com/download?id=XYZ&export=download"));
    CHECK(!is_drive_url("https://example.com/file/d/abc"));
    CHECK(!is_drive_url("https://notdrive.google.com.evil.com/x"));

    auto a = drive_file_id("https://drive.google.com/file/d/1A2B3C4D5E6F7g/view?usp=sharing");
    CHECK(a && *a == "1A2B3C4D5E6F7g");
    auto b = drive_file_id("https://drive.google.com/open?id=ZZZ_file_id_123");
    CHECK(b && *b == "ZZZ_file_id_123");
    auto c = drive_file_id("https://drive.usercontent.google.com/download?id=qq_longid_99&export=download");
    CHECK(c && *c == "qq_longid_99");
    CHECK(!drive_file_id("https://drive.google.com/").has_value());
}

static void test_native_framing() {
    // Round-trip a message through the 4-byte-LE length framing.
    std::stringstream buf;
    std::string body = R"({"type":"download","url":"https://h/x","connections":4})";
    write_frame(buf, body);
    // The prefix is the little-endian byte length.
    std::string raw = buf.str();
    CHECK(raw.size() == body.size() + 4);
    CHECK(static_cast<unsigned char>(raw[0]) == (body.size() & 0xFF));
    auto got = read_frame(buf);
    CHECK(got.has_value());
    CHECK(*got == body);
    // A truncated stream yields nullopt rather than a bogus message.
    std::stringstream half;
    half.write(raw.data(), 2);
    CHECK(!read_frame(half).has_value());

    CHECK(is_stream_job("https://h/v/index.m3u8", false));
    CHECK(is_stream_job("https://h/v/manifest.mpd", false));
    CHECK(is_stream_job("https://h/plain.bin", true));   // explicit flag
    CHECK(!is_stream_job("https://h/plain.bin", false));
}

static void test_manager_persistence() {
    auto tmp = (fs::temp_directory_path() /
                fs::path("mpdm_prefs_" + std::to_string(::time(nullptr)))).string();
    fs::create_directories(tmp);
    set_home(tmp);

    {
        mpdm::DownloadManager m(mpdm::Settings{}, 2, 0);
        m.settings.connections = 11;
        m.settings.output_dir = "D:/Downloads/test";
        m.max_concurrent = 5;
        m.set_global_rate(750 * 1024);
        m.set_on_complete("shutdown", "");
        m.clip_monitor_pref = true;
        m.save_settings();
        m.add_schedule("resume_all", "01:30");
        m.add_schedule("pause_all", "07:00");
    }

    // A fresh manager reloads settings.json + schedule.json.
    {
        mpdm::DownloadManager m2(mpdm::Settings{}, 2, 0);
        CHECK(m2.settings.connections == 11);
        CHECK(m2.settings.output_dir == "D:/Downloads/test");
        CHECK(m2.max_concurrent == 5);
        CHECK(m2.global_rate_bps() == 750 * 1024);
        CHECK(m2.on_done_action == "shutdown");
        CHECK(m2.clip_monitor_pref == true);
        CHECK(m2.list_schedules().size() == 2);
    }

    std::error_code ec;
    fs::remove_all(tmp, ec);
}

static void test_scheduler() {
    std::tm t{};
    t.tm_year = 2026 - 1900; t.tm_mon = 5; t.tm_mday = 15;  // 2026-06-15
    t.tm_hour = 12; t.tm_min = 0; t.tm_sec = 0; t.tm_isdst = -1;
    std::time_t noon = std::mktime(&t);

    ScheduleRule daily;
    daily.action = "resume_all"; daily.at = "12:00"; daily.repeat = "daily"; daily.enabled = true;
    CHECK(rule_fires_between(daily, noon - 60, noon + 60));    // crosses 12:00 -> fires
    CHECK(!rule_fires_between(daily, noon + 60, noon + 120));  // already past
    CHECK(!rule_fires_between(daily, noon - 120, noon - 60));  // not yet
    daily.enabled = false;
    CHECK(!rule_fires_between(daily, noon - 60, noon + 60));   // disabled -> never

    // Weekday filter (computed, so the test isn't calendar-fragile).
    std::tm lt = *std::localtime(&noon);
    int pywd = (lt.tm_wday + 6) % 7;  // 0=Mon..6=Sun
    ScheduleRule dfilter;
    dfilter.action = "resume_all"; dfilter.at = "12:00"; dfilter.enabled = true;
    dfilter.days = {pywd};
    CHECK(rule_fires_between(dfilter, noon - 60, noon + 60));
    dfilter.days = {(pywd + 1) % 7};
    CHECK(!rule_fires_between(dfilter, noon - 60, noon + 60));

    // One-shot ISO rule.
    ScheduleRule once;
    once.action = "pause_all"; once.at = "2026-06-15T12:00"; once.repeat = "once"; once.enabled = true;
    CHECK(rule_fires_between(once, noon - 60, noon + 60));
    CHECK(!rule_fires_between(once, noon + 60, noon + 120));

    // Edge-triggered firing via the Scheduler with an injected clock.
    int fired = 0;
    Scheduler sch({{"resume_all", [&](const ScheduleRule&) { ++fired; }}}, /*store=*/"");
    sch.add_rule("resume_all", "12:00");
    sch.tick(noon - 120);  // first tick = baseline, no backfire
    CHECK(fired == 0);
    sch.tick(noon + 60);   // crosses 12:00 -> fires once
    CHECK(fired == 1);
    sch.tick(noon + 120);  // no re-cross -> no refire
    CHECK(fired == 1);

    // An unknown action is rejected up front.
    bool threw = false;
    try { sch.add_rule("not_an_action", "01:00"); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
}

// Failed download -> New link -> Retry, end to end over a local server. The "old"
// link 403s for the second half (an expired token), so the download fails with the
// first half saved. Pointing it at the "new" link must continue (fetch only the
// missing half), produce identical bytes, and still move out of the temp folder.
static void test_retry_new_link() {
    auto tmp = (fs::temp_directory_path() / fs::path("mpdm_retry_" + std::to_string(::time(nullptr)))).string();
    fs::create_directories(tmp);
    set_home(tmp);

    const long long kSize = 2 * 1024 * 1024, kHalf = kSize / 2;
    std::string body(static_cast<size_t>(kSize), '\0');
    for (long long i = 0; i < kSize; ++i) body[static_cast<size_t>(i)] = static_cast<char>((i * 131 + 7) % 251);

    std::mutex mu;
    long long new_bytes = 0;  // body bytes the new link served (excluding the 1-byte probe)
    httplib::Server svr;
    auto serve = [&](const httplib::Request& req, httplib::Response& res, bool expired,
                     const char* etag = "\"v1\"") {
        long long a = 0, b = kSize - 1;
        if (!req.ranges.empty()) {
            a = req.ranges[0].first;
            if (req.ranges[0].second >= 0) b = req.ranges[0].second;
        }
        res.set_header("ETag", etag);
        res.set_header("Accept-Ranges", "bytes");
        if (expired && a >= kHalf) {  // an error *page*, like real sites send
            res.status = 403;
            res.set_content("<html><body>403 Forbidden: link expired</body></html>", "text/html");
            return;
        }
        if (!expired && b > a) { std::lock_guard<std::mutex> lk(mu); new_bytes += b - a + 1; }
        res.set_content(body, "application/octet-stream");  // httplib answers the Range (206)
    };
    svr.Get("/old/f.bin", [&](const httplib::Request& q, httplib::Response& r) { serve(q, r, true); });
    svr.Get("/new/f.bin", [&](const httplib::Request& q, httplib::Response& r) { serve(q, r, false); });
    // Same bytes, different ETag: a mirror (or a changed file) that can't be told apart.
    svr.Get("/mirror/f.bin", [&](const httplib::Request& q, httplib::Response& r) { serve(q, r, false, "\"v2\""); });
    int port = svr.bind_to_any_port("127.0.0.1");
    std::thread th([&] { svr.listen_after_bind(); });
    std::string base = "http://127.0.0.1:" + std::to_string(port);

    Settings s;
    s.output_dir = (fs::path(tmp) / "out").string();
    s.temp_dir = (fs::path(tmp) / "work").string();
    s.categorize = false;
    s.connections = 4;
    s.min_segment_size = 256 * 1024;
    s.dynamic_segmentation = false;  // fixed 4 x 512 KiB ranges: two succeed, two 403
    s.max_retries = 0;
    {
        DownloadManager m(s, 1, 0);
        auto it = m.add(base + "/old/f.bin", "f.bin", 4, {}, /*start=*/true);
        auto wait_for = [&](const char* st) {
            for (int i = 0; i < 300 && it->status != st; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return it->status == st;
        };
        CHECK(wait_for(ST_ERROR));  // the 403s end in "failed", not an endless re-request
        CHECK(it->error.find("403") != std::string::npos);
        CHECK(it->downloaded == kHalf);  // first half kept; the error pages wrote nothing
        CHECK(it->dest_path == (fs::path(s.output_dir) / "f.bin").string());  // real target, not the temp copy

        CHECK(m.set_url(it->id, base + "/new/f.bin"));
        m.resume(it->id);
        CHECK(wait_for(ST_COMPLETED));
        CHECK(new_bytes > 0 && new_bytes <= kHalf);  // continued, didn't start over

        std::string got;
        { std::ifstream f(it->dest_path, std::ios::binary); std::ostringstream o; o << f.rdbuf(); got = o.str(); }
        CHECK(fs::path(it->dest_path).parent_path() == fs::path(s.output_dir));
        CHECK(got == body);
        CHECK(!fs::exists(fs::path(s.temp_dir) / "f.bin"));

        // The server's file now looks different (ETag v1 -> v2): progress is never
        // dropped silently. "keep" stops with the partial intact (and no auto-retry);
        // "continue" then resumes it; the expected checksum is verified at the end.
        std::string choice = "keep";
        int asked = 0;
        ResumeConflict seen;
        m.set_resume_resolver([&](const ResumeConflict& c) { ++asked; seen = c; return choice; });
        auto g = m.add(base + "/old/f.bin", "g.bin", 4, {}, true);
        auto wait_g = [&](const char* st) {
            for (int i = 0; i < 300 && g->status != st; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return g->status == st;
        };
        CHECK(wait_g(ST_ERROR));
        CHECK(g->downloaded == kHalf);
        CHECK(m.set_url(g->id, base + "/mirror/f.bin"));
        m.resume(g->id);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(wait_g(ST_ERROR));
        CHECK(asked == 1);
        CHECK(seen.have == kHalf && !seen.size_changed);
        CHECK(g->no_auto_retry);
        CHECK(g->error.find("looks different") != std::string::npos);
        CHECK(m.auto_retry_in(*g) < 0);
        CHECK(fs::exists(sidecar_path((fs::path(s.temp_dir) / "g.bin").string())));

        std::string sum;
        {
            auto ref = (fs::path(tmp) / "ref.bin").string();
            { std::ofstream f(ref, std::ios::binary); f << body; }
            sum = file_digest(ref, "sha256");
        }
        CHECK(m.set_expected_hash(g->id, "SHA-256: " + sum));
        choice = "continue";
        long long before = new_bytes;
        m.resume(g->id);
        CHECK(wait_g(ST_COMPLETED));
        CHECK(asked == 2);
        CHECK(new_bytes - before <= kHalf);  // continued from the saved half
        CHECK(g->hash_state == "ok");
        {
            std::ifstream f(g->dest_path, std::ios::binary); std::ostringstream o; o << f.rdbuf();
            CHECK(o.str() == body);
        }

        // A wrong checksum on a finished file: checked in the background -> failed,
        // and not retried automatically.
        CHECK(!m.set_expected_hash(g->id, "not-a-checksum"));
        CHECK(m.set_expected_hash(g->id, std::string(64, '0')));
        for (int i = 0; i < 200 && g->hash_state != "mismatch"; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(g->hash_state == "mismatch");
        CHECK(g->status == ST_ERROR && g->no_auto_retry);
    }
    svr.stop();
    th.join();
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

static void test_checksum() {
    CHECK(parse_checksum("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD").algo == "sha256");
    CHECK(parse_checksum("sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad").valid());
    CHECK(parse_checksum("SHA-256 ba7816bf 8f01cfea 414140de 5dae2223 b00361a3 96177a9c b410ff61 f20015ad").valid());
    CHECK(parse_checksum("900150983cd24fb0d6963f7d28e17f72  ubuntu.iso").algo == "md5");  // SHA256SUMS-style line
    CHECK(parse_checksum("a9993e364706816aba3e25717850c26c9cd0d89d").algo == "sha1");
    CHECK(!parse_checksum("sha1:900150983cd24fb0d6963f7d28e17f72").valid());  // md5 length, sha1 prefix
    CHECK(!parse_checksum("hello").valid());
    CHECK(!parse_checksum("").valid());

    auto p = (fs::temp_directory_path() / "mpdm_abc.txt").string();
    { std::ofstream f(p, std::ios::binary); f << "abc"; }
    CHECK(file_digest(p, "sha256") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(file_digest(p, "md5") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(file_digest(p, "sha1") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(file_digest(p, "nope").empty());
    CHECK(file_digest(p + ".missing", "sha256").empty());
    std::error_code ec;
    fs::remove(p, ec);
}

// Auto-retry timing + give-up, and resume-on-start, without any network
// (max_concurrent = 0, so requeued items never actually start).
static void test_auto_retry_and_resume_on_start() {
    auto tmp = (fs::temp_directory_path() / fs::path("mpdm_auto_" + std::to_string(::time(nullptr)))).string();
    fs::create_directories(tmp);
    set_home(tmp);
    Settings s;
    s.auto_retry_minutes = 5;
    s.auto_retry_hours = 24;
    {
        DownloadManager m(s, 0, 0);
        auto a = m.add("http://127.0.0.1:1/a.bin", "", 0, {}, false);
        auto b = m.add("http://127.0.0.1:1/b.bin", "", 0, {}, false);
        auto c = m.add("http://127.0.0.1:1/c.bin", "", 0, {}, false);
        const double t = 1000000;
        a->status = ST_ERROR; a->failed_since = t; a->failed_at = t;                            // normal failure
        b->status = ST_ERROR; b->failed_since = t; b->failed_at = t; b->no_auto_retry = true;   // stopped on purpose
        c->status = ST_ERROR; c->failed_since = t - 25 * 3600; c->failed_at = t;                // failing > 24 h: gave up
        CHECK(m.auto_retry_in(*a, t + 60) == 240);
        CHECK(m.retry_failed_due(t + 299) == 0);
        CHECK(m.retry_failed_due(t + 300) == 1);
        CHECK(a->status == ST_QUEUED && a->failed_since == t);  // streak kept for the give-up clock
        CHECK(b->status == ST_ERROR && c->status == ST_ERROR);
        CHECK(m.auto_retry_in(*b, t) < 0 && m.auto_retry_in(*c, t) < 0);
        m.settings.auto_retry_minutes = 0;  // off
        a->status = ST_ERROR;
        CHECK(m.retry_failed_due(t + 10000) == 0);

        // A manual Retry resets the streak and the "stopped on purpose" flag.
        m.resume(b->id);
        CHECK(b->status == ST_QUEUED && b->failed_since == 0 && !b->no_auto_retry);

        // Persisted as running/queued vs paused for the restart below.
        a->status = ST_QUEUED; b->status = ST_DOWNLOADING; c->status = ST_PAUSED;
        m.save();
    }
    {
        DownloadManager m(s, 0, 0);  // resume_on_start defaults to on
        CHECK(m.resume_interrupted() == 2);
        int queued = 0;
        for (auto& it : m.ordered()) {
            if (it->url.find("c.bin") != std::string::npos) CHECK(it->status == ST_PAUSED);
            else if (it->status == ST_QUEUED) ++queued;
        }
        CHECK(queued == 2);
        CHECK(m.resume_interrupted() == 0);  // only once per run
        for (auto& it : m.ordered()) if (it->status == ST_QUEUED) it->status = ST_DOWNLOADING;
        m.save();
    }
    {
        Settings off = s;
        off.resume_on_start = false;
        DownloadManager m(off, 0, 0);
        CHECK(m.resume_interrupted() == 0);
        for (auto& it : m.ordered()) CHECK(it->status == ST_PAUSED);
    }
    std::error_code ec;
    fs::remove_all(tmp, ec);
}

int main() {
    test_plan_segments();
    test_checksum();
    test_auto_retry_and_resume_on_start();
    test_parse_rate();
    test_categories();
    test_batch();
    test_storage_roundtrip();
    test_manager();
    test_aes();
    test_hls();
    test_dash();
    test_grabber_plan();
    test_config_ua();
    test_clean();
    test_gdrive();
    test_native_framing();
    test_scheduler();
    test_manager_persistence();
    test_retry_new_link();
    if (g_failures == 0) {
        std::cout << "all C++ backend tests passed\n";
        return 0;
    }
    std::cerr << g_failures << " check(s) failed\n";
    return 1;
}
