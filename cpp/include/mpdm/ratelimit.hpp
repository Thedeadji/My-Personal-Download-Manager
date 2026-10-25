// Bandwidth throttling — port of mpdm/ratelimit.py.
#pragma once
#include <mutex>
#include <string>

namespace mpdm {

// Thread-safe token-bucket limiter. rate <= 0 means unlimited (consume is a no-op).
class RateLimiter {
public:
    explicit RateLimiter(double rate_bps = 0.0);
    double rate() const;
    void set_rate(double rate_bps);
    void consume(long long nbytes);   // blocks as needed to honor the rate

private:
    double rate_;
    double tokens_;
    double last_;                     // monotonic seconds
    mutable std::mutex m_;
};

// "500k" / "1.5m" / "2g" / "1000000" -> bytes/sec (binary units). Empty / "0" /
// "unlimited" / "none" -> 0. Throws std::invalid_argument on garbage.
long long parse_rate(const std::string& text);

// Render a bytes/sec rate, e.g. "1.5 MB/s" or "unlimited".
std::string format_rate(double bps);

} // namespace mpdm
