// MyPersonalDownloadManager — C++ port entry point (early).
//
// With no args: prints a short status. With a URL arg: probes it (HTTP layer
// smoke test) and prints what it learned. Real code is ported bottom-up per
// ../PORTING.md; the engine (which uses this HTTP layer) comes next.

#include <iostream>

#include "mpdm/http_client.hpp"
#include "mpdm/ratelimit.hpp"
#include "mpdm/segments.hpp"

int main(int argc, char** argv) {
    if (argc > 1) {
        try {
            mpdm::RemoteInfo i = mpdm::probe(argv[1]);
            std::cout << "probe " << argv[1] << "\n"
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

    std::cout << "MyPersonalDownloadManager (C++) — early port\n";
    auto segs = mpdm::plan_segments(100'000'000, 8, 256 * 1024);
    std::cout << "plan_segments(100MB, 8) -> " << segs.size() << " segments\n";
    std::cout << "rate 1.5m -> " << mpdm::format_rate((double)mpdm::parse_rate("1.5m")) << "\n";
    std::cout << "(pass a URL to probe it)\n";
    return 0;
}
