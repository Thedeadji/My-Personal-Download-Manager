#include "mpdm/ratelimit.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace mpdm {

static double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

RateLimiter::RateLimiter(double rate_bps)
    : rate_(rate_bps > 0 ? rate_bps : 0.0), tokens_(0.0), last_(now_seconds()) {}

double RateLimiter::rate() const {
    std::lock_guard<std::mutex> lk(m_);
    return rate_;
}

void RateLimiter::set_rate(double rate_bps) {
    std::lock_guard<std::mutex> lk(m_);
    rate_ = rate_bps > 0 ? rate_bps : 0.0;
    last_ = now_seconds();
    tokens_ = 0.0;
}

void RateLimiter::consume(long long nbytes) {
    if (nbytes <= 0) return;
    double wait = 0.0;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (rate_ <= 0) return;
        double now = now_seconds();
        tokens_ = std::min(rate_, tokens_ + (now - last_) * rate_);  // cap ~1s burst
        last_ = now;
        tokens_ -= static_cast<double>(nbytes);
        if (tokens_ < 0) wait = -tokens_ / rate_;
    }
    if (wait > 0) std::this_thread::sleep_for(std::chrono::duration<double>(wait));
}

long long parse_rate(const std::string& text) {
    std::string s;
    s.reserve(text.size());
    for (char c : text) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    // trim
    const char* ws = " \t\n\r";
    size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return 0;
    size_t b = s.find_last_not_of(ws);
    s = s.substr(a, b - a + 1);
    // drop "/s" and "ps"
    auto erase_all = [&](const std::string& sub) {
        size_t p;
        while ((p = s.find(sub)) != std::string::npos) s.erase(p, sub.size());
    };
    erase_all("/s");
    erase_all("ps");
    if (s.empty() || s == "0" || s == "none" || s == "unlimited" || s == "inf") return 0;
    if (!s.empty() && s.back() == 'b') s.pop_back();   // kb, mb, …
    if (!s.empty() && s.back() == 'i') s.pop_back();   // kib, mib, …
    long long mult = 1;
    if (!s.empty()) {
        switch (s.back()) {
            case 'k': mult = 1024LL; break;
            case 'm': mult = 1024LL * 1024; break;
            case 'g': mult = 1024LL * 1024 * 1024; break;
            case 't': mult = 1024LL * 1024 * 1024 * 1024; break;
            default: break;
        }
        if (mult != 1) s.pop_back();
    }
    try {
        size_t used = 0;
        double v = std::stod(s, &used);
        if (used != s.size()) throw std::invalid_argument("trailing chars");
        long long r = static_cast<long long>(v * static_cast<double>(mult));
        return r > 0 ? r : 0;
    } catch (...) {
        throw std::invalid_argument("invalid rate: " + text);
    }
}

std::string format_rate(double bps) {
    if (bps <= 0) return "unlimited";
    double n = bps;
    const char* units[] = {"B", "KB", "MB", "GB"};
    char buf[64];
    for (const char* u : units) {
        if (n < 1024.0) {
            std::snprintf(buf, sizeof(buf), "%.1f %s/s", n, u);
            return buf;
        }
        n /= 1024.0;
    }
    std::snprintf(buf, sizeof(buf), "%.1f TB/s", n);
    return buf;
}

} // namespace mpdm
