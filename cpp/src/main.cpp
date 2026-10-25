// MyPersonalDownloadManager — C++ port entry point (early scaffold).
//
// Real code is ported bottom-up per ../PORTING.md. Ported so far (dependency-free):
// models, segment planning, rate limiting, categories, batch/wildcard. Next: the
// HTTP layer (libcurl) once a git-cloned vcpkg is set up.

#include <iostream>

#include "mpdm/ratelimit.hpp"
#include "mpdm/segments.hpp"

int main() {
    std::cout << "MyPersonalDownloadManager (C++) — early port\n";
    auto segs = mpdm::plan_segments(100'000'000, 8, 256 * 1024);
    std::cout << "plan_segments(100MB, 8) -> " << segs.size() << " segments\n";
    std::cout << "rate parse/format: 1.5m -> "
              << mpdm::format_rate(static_cast<double>(mpdm::parse_rate("1.5m"))) << "\n";
    return 0;
}
