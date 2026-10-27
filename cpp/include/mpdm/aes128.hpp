// AES-128-CBC decryption (HLS segments) — port of mpdm/aes128.py, using OpenSSL.
#pragma once
#include <string>

namespace mpdm {

// Decrypt AES-128-CBC. key and iv must be 16 bytes; data a multiple of 16.
// Strips PKCS#7 padding by default (leniently, like the Python version).
std::string aes128_cbc_decrypt(const std::string& key, const std::string& iv,
                               const std::string& data, bool unpad = true);

// Remove PKCS#7 padding; returns data unchanged if the padding looks invalid.
std::string pkcs7_unpad(const std::string& data);

} // namespace mpdm
