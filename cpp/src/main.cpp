// MyPersonalDownloadManager — C++ port entry point (early).
//
// With no args: prints a short status. With a URL arg: probes it (HTTP layer
// smoke test) and prints what it learned. Real code is ported bottom-up per
// ../PORTING.md; the engine (which uses this HTTP layer) comes next.

#include <iostream>
#include <string>

#include <chrono>
#include <thread>

#include "mpdm/config.hpp"
#include "mpdm/engine.hpp"
#include "mpdm/grabber.hpp"
#include "mpdm/http_client.hpp"
#include "mpdm/manager.hpp"
#include "mpdm/ratelimit.hpp"
#include "mpdm/segments.hpp"

static int do_probe(const char* url) {
    try {
        mpdm::RemoteInfo i = mpdm::probe(url);
        std::cout << "probe " << url << "\n"
                  << "  final_url    : " << i.final_url << "\n"
                  << "  size         : " << (i.size ? std::to_string(*i.size) : "unknown") << "\n"
                  << "  accept_ranges: " << (i.accept_ranges ? "yes" : "no") << "\n"
                  << "  filename     : " << i.filename << "\n"
                  << "  content_type : " << (i.content_type ? *i.content_type : "") << "\n";
    } catch (const std::exception& e) {
        std::cerr << "probe error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

static int do_get(const char* url, const std::string& dest) {
    mpdm::Settings s;
    s.output_dir = ".";
    try {
        mpdm::Download dl(url, s, dest,
                          [](const mpdm::Progress& p) {
                              if (auto pct = p.percent())
                                  std::cout << "\r  " << (int)*pct << "%  "
                                            << (p.speed_bps / 1024.0) << " KB/s   " << std::flush;
                          });
        std::string path = dl.start();
        std::cout << "\nsaved to " << path << "\n";
    } catch (const std::exception& e) {
        std::cerr << "\nget error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

static int do_queue(const char* url) {
    mpdm::Settings s;
    s.output_dir = ".";
    mpdm::DownloadManager mgr(s, 3, 0);
    auto it = mgr.add(url);
    while (it->status == mpdm::ST_QUEUED || it->status == mpdm::ST_DOWNLOADING) {
        if (auto pct = it->percent())
            std::cout << "\r  " << (int)*pct << "%  " << (it->speed / 1024.0) << " KB/s   " << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::cout << "\n  status: " << it->status;
    if (!it->error.empty()) std::cout << " (" << it->error << ")";
    std::cout << "\n  file  : " << it->dest_path << "\n";
    return it->status == mpdm::ST_COMPLETED ? 0 : 1;
}

static int do_grab(const char* url, const std::string& out) {
    std::string dest = out.empty() ? "out.mp4" : out;
    mpdm::Settings s;
    try {
        mpdm::grab(url, dest, s, "auto", "best",
                   [](int done, int total) {
                       std::cout << "\r  segments " << done << "/" << total << "   " << std::flush;
                   });
        std::cout << "\nsaved to " << dest << "\n";
    } catch (const std::exception& e) {
        std::cerr << "\ngrab error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "probe") return do_probe(argv[2]);
    if (argc >= 3 && std::string(argv[1]) == "get")
        return do_get(argv[2], argc >= 4 ? argv[3] : "");
    if (argc >= 3 && std::string(argv[1]) == "grab")
        return do_grab(argv[2], argc >= 4 ? argv[3] : "");
    if (argc >= 3 && std::string(argv[1]) == "queue") return do_queue(argv[2]);
    if (argc == 2) return do_probe(argv[1]);  // bare URL -> probe

    std::cout << "MyPersonalDownloadManager (C++) — early port\n";
    auto segs = mpdm::plan_segments(100'000'000, 8, 256 * 1024);
    std::cout << "plan_segments(100MB, 8) -> " << segs.size() << " segments\n";
    std::cout << "rate 1.5m -> " << mpdm::format_rate((double)mpdm::parse_rate("1.5m")) << "\n";
    std::cout << "usage: mpdm probe <url> | mpdm get <url> [dest] | "
                 "mpdm grab <m3u8|mpd> [out.mp4]\n";
    return 0;
}
