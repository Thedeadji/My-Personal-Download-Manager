// MyPersonalDownloadManager — C++ port entry point (early).
//
// With no args: prints a short status. With a URL arg: probes it (HTTP layer
// smoke test) and prints what it learned. Real code is ported bottom-up per
// ../PORTING.md; the engine (which uses this HTTP layer) comes next.

#include <iostream>
#include <string>

#include "mpdm/config.hpp"
#include "mpdm/engine.hpp"
#include "mpdm/http_client.hpp"
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

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "probe") return do_probe(argv[2]);
    if (argc >= 3 && std::string(argv[1]) == "get")
        return do_get(argv[2], argc >= 4 ? argv[3] : "");
    if (argc == 2) return do_probe(argv[1]);  // bare URL -> probe

    std::cout << "MyPersonalDownloadManager (C++) — early port\n";
    auto segs = mpdm::plan_segments(100'000'000, 8, 256 * 1024);
    std::cout << "plan_segments(100MB, 8) -> " << segs.size() << " segments\n";
    std::cout << "rate 1.5m -> " << mpdm::format_rate((double)mpdm::parse_rate("1.5m")) << "\n";
    std::cout << "usage: mpdm probe <url> | mpdm get <url> [dest]\n";
    return 0;
}
