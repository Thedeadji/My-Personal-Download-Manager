// Backend unit tests — validate the ported logic against the Python behavior.
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "mpdm/batch.hpp"
#include "mpdm/categories.hpp"
#include "mpdm/manager.hpp"
#include "mpdm/models.hpp"
#include "mpdm/ratelimit.hpp"
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

    std::map<std::string, std::string> ov = {{"Video", (fs::path("D:") / "Movies").string()},
                                             {"Audio", "tunes"}};
    CHECK(fs::path(categorized_dir(base, "a.mp4", true, ov)) == fs::path("D:") / "Movies");  // abs
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

int main() {
    test_plan_segments();
    test_parse_rate();
    test_categories();
    test_batch();
    test_storage_roundtrip();
    test_manager();
    if (g_failures == 0) {
        std::cout << "all C++ backend tests passed\n";
        return 0;
    }
    std::cerr << g_failures << " check(s) failed\n";
    return 1;
}
