#include "mpdm/checksum.hpp"

#include <openssl/evp.h>

#include <cctype>
#include <fstream>
#include <vector>

namespace mpdm {

static Checksum parse_one(const std::string& text);

Checksum parse_checksum(const std::string& text) {
    // "SHA-256 abcd…" / "sha256:abcd…" read as a whole; a "<hash>  file.iso" line
    // from a SHA256SUMS file falls back to its first word that is a checksum.
    Checksum whole = parse_one(text);
    if (whole.valid()) return whole;
    std::string word;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || std::isspace(static_cast<unsigned char>(text[i]))) {
            if (!word.empty()) {
                Checksum c = parse_one(word);
                if (c.valid()) return c;
                word.clear();
            }
        } else {
            word += text[i];
        }
    }
    return {};
}

static Checksum parse_one(const std::string& text) {
    std::string s;
    for (char c : text)
        if (!std::isspace(static_cast<unsigned char>(c))) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Optional algorithm prefix: "sha256:", "sha-256:", "sha256=" or "sha256" glued on.
    std::string algo;
    static const char* names[][2] = {{"sha-512", "sha512"}, {"sha512", "sha512"}, {"sha-256", "sha256"},
                                     {"sha256", "sha256"},  {"sha-1", "sha1"},     {"sha1", "sha1"},
                                     {"md5", "md5"}};
    for (auto& n : names) {
        std::string p = n[0];
        if (s.compare(0, p.size(), p) == 0) {
            algo = n[1];
            s = s.substr(p.size());
            if (!s.empty() && (s[0] == ':' || s[0] == '=')) s = s.substr(1);
            break;
        }
    }
    if (s.empty()) return {};
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return {};

    const std::string by_len = s.size() == 32 ? "md5" : s.size() == 40 ? "sha1"
                             : s.size() == 64 ? "sha256" : s.size() == 128 ? "sha512" : "";
    if (by_len.empty() || (!algo.empty() && algo != by_len)) return {};  // wrong length for the prefix
    return {by_len, s};
}

std::string file_digest(const std::string& path, const std::string& algo, const std::atomic<bool>* stop) {
    const EVP_MD* md = algo == "md5" ? EVP_md5() : algo == "sha1" ? EVP_sha1()
                     : algo == "sha256" ? EVP_sha256() : algo == "sha512" ? EVP_sha512() : nullptr;
    if (!md) return "";
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestInit_ex(ctx, md, nullptr) != 1) { EVP_MD_CTX_free(ctx); return ""; }

    std::vector<char> buf(1 << 20);
    bool ok = true;
    while (ok && f) {
        if (stop && *stop) { ok = false; break; }
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::streamsize n = f.gcount();
        if (n > 0) ok = EVP_DigestUpdate(ctx, buf.data(), static_cast<size_t>(n)) == 1;
    }
    if (f.bad()) ok = false;

    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (ok) ok = EVP_DigestFinal_ex(ctx, out, &len) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok) return "";

    static const char* hexd = "0123456789abcdef";
    std::string hex;
    for (unsigned int i = 0; i < len; ++i) { hex += hexd[out[i] >> 4]; hex += hexd[out[i] & 15]; }
    return hex;
}

std::string checksum_label(const std::string& algo) {
    if (algo == "md5") return "MD5";
    if (algo == "sha1") return "SHA-1";
    if (algo == "sha256") return "SHA-256";
    if (algo == "sha512") return "SHA-512";
    return algo;
}

} // namespace mpdm
