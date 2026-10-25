#include "mpdm/batch.hpp"

#include <regex>
#include <stdexcept>
#include <unordered_set>

namespace mpdm {

// Matches [1-100] (numeric) or [a-e] (alpha).
static const std::regex kRange(R"(\[(?:(\d+)-(\d+)|([a-zA-Z])-([a-zA-Z]))\])");

std::vector<std::string> expand_pattern(const std::string& url) {
    std::smatch m;
    if (!std::regex_search(url, m, kRange)) return {url};

    const std::string head = m.prefix().str();
    const std::string tail = m.suffix().str();

    std::vector<std::string> tokens;
    if (m[1].matched) {  // numeric range
        const std::string a = m[1].str(), b = m[2].str();
        long long start = std::stoll(a), end = std::stoll(b);
        int step = (end >= start) ? 1 : -1;
        int width = (!a.empty() && a[0] == '0') ? static_cast<int>(a.size()) : 0;
        for (long long n = start; (step > 0 ? n <= end : n >= end); n += step) {
            std::string s = std::to_string(n);
            if (width > 0 && static_cast<int>(s.size()) < width)
                s = std::string(width - s.size(), '0') + s;
            tokens.push_back(std::move(s));
        }
    } else {  // alpha range
        char a = m[3].str()[0], b = m[4].str()[0];
        int step = (b >= a) ? 1 : -1;
        for (char c = a; (step > 0 ? c <= b : c >= b); c = static_cast<char>(c + step))
            tokens.emplace_back(1, c);
    }

    std::vector<std::string> out;
    for (const auto& tok : tokens) {
        for (auto& rest : expand_pattern(head + tok + tail)) {
            out.push_back(std::move(rest));
            if (out.size() > kMaxExpansion)
                throw std::length_error("pattern expands to more than 10000 URLs");
        }
    }
    return out;
}

std::vector<std::string> expand_urls(const std::string& text) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    std::regex sep(R"([\s,]+)");
    std::sregex_token_iterator it(text.begin(), text.end(), sep, -1), endit;
    for (; it != endit; ++it) {
        std::string part = *it;
        if (part.empty()) continue;
        for (auto& u : expand_pattern(part)) {
            if (seen.insert(u).second) out.push_back(std::move(u));
        }
    }
    return out;
}

} // namespace mpdm
