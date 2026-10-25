// Segment planning — port of engine.plan_segments().
#pragma once
#include <algorithm>
#include <vector>

#include "mpdm/models.hpp"

namespace mpdm {

// Divide [0, size) into up to `connections` contiguous segments, each at least
// `min_segment_size` where possible.
inline std::vector<Segment> plan_segments(long long size, int connections,
                                          long long min_segment_size) {
    std::vector<Segment> segs;
    if (size <= 0) {
        segs.push_back(Segment{0, 0, -1, 0});  // unknown/empty — caller handles
        return segs;
    }
    long long max_by_min = (size + min_segment_size - 1) / min_segment_size;
    int n = static_cast<int>(std::max<long long>(1, std::min<long long>(connections, max_by_min)));
    long long base = size / n;
    long long start = 0;
    for (int i = 0; i < n; ++i) {
        long long end = (i == n - 1) ? size - 1 : start + base - 1;
        segs.push_back(Segment{i, start, end, 0});
        start = end + 1;
    }
    return segs;
}

} // namespace mpdm
