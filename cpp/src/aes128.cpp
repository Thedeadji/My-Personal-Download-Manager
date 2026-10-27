#include "mpdm/aes128.hpp"

#include <openssl/evp.h>

#include <stdexcept>

namespace mpdm {

std::string pkcs7_unpad(const std::string& data) {
    if (data.empty()) return data;
    unsigned char pad = static_cast<unsigned char>(data.back());
    if (pad >= 1 && pad <= 16 && data.size() >= pad) {
        for (size_t i = data.size() - pad; i < data.size(); ++i)
            if (static_cast<unsigned char>(data[i]) != pad) return data;  // invalid -> unchanged
        return data.substr(0, data.size() - pad);
    }
    return data;
}

std::string aes128_cbc_decrypt(const std::string& key, const std::string& iv,
                               const std::string& data, bool unpad) {
    if (key.size() != 16) throw std::invalid_argument("AES-128 key must be 16 bytes");
    if (iv.size() != 16) throw std::invalid_argument("IV must be 16 bytes");
    if (data.size() % 16 != 0) throw std::invalid_argument("ciphertext length must be a multiple of 16");

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");

    std::string out;
    out.resize(data.size() + 16);
    int outlen = 0, total = 0;
    bool ok =
        EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr,
                           reinterpret_cast<const unsigned char*>(key.data()),
                           reinterpret_cast<const unsigned char*>(iv.data())) == 1 &&
        // Handle PKCS#7 ourselves (leniently) to match the Python behavior.
        EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
        EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(&out[0]), &outlen,
                          reinterpret_cast<const unsigned char*>(data.data()),
                          static_cast<int>(data.size())) == 1;
    total = outlen;
    if (ok) {
        ok = EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(&out[0]) + total, &outlen) == 1;
        total += outlen;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) throw std::runtime_error("AES-128-CBC decrypt failed");

    out.resize(total);
    return unpad ? pkcs7_unpad(out) : out;
}

} // namespace mpdm
