// File checksums: verify a finished download against the hash a site publishes.
#pragma once
#include <atomic>
#include <string>

namespace mpdm {

struct Checksum {
    std::string algo;  // "md5" | "sha1" | "sha256" | "sha512"; "" = not a checksum
    std::string hex;   // lowercase hex digest
    bool valid() const { return !algo.empty(); }
    std::string str() const { return valid() ? algo + ":" + hex : std::string(); }
};

// Read a pasted checksum: spaces ignored, an optional "sha256:" / "SHA-256 " style
// prefix allowed, case-insensitive. Without a prefix the algorithm is picked from
// the length (32 md5, 40 sha1, 64 sha256, 128 sha512). Invalid -> algo "".
Checksum parse_checksum(const std::string& text);

// Hash a file with `algo` and return the lowercase hex digest; "" on a read error,
// an unknown algorithm, or when `stop` becomes true (checked between chunks).
std::string file_digest(const std::string& path, const std::string& algo,
                        const std::atomic<bool>* stop = nullptr);

// Display name: "sha256" -> "SHA-256".
std::string checksum_label(const std::string& algo);

} // namespace mpdm
