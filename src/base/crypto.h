// The little crypto the app itself needs (OAuth PKCE, WebSocket keys,
// nonces) — no TLS here: that is the OS's, or mbedTLS inside net/ on Linux.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace crypto {

std::array<uint8_t, 32> sha256(std::string_view data);
std::array<uint8_t, 20> sha1(std::string_view data);
std::array<uint8_t, 20> hmacSha1(std::string_view key, std::string_view data);
// PBKDF2-HMAC-SHA1 (RFC 2898). `len` bytes of derived key. Used only for
// Chromium's fixed cookie-key derivation (1 iteration), so it is not a
// password hash and makes no timing promises beyond HMAC-SHA1's.
std::string
pbkdf2Sha1(std::string_view password, std::string_view salt, int iterations, size_t len);

// From the OS's CSPRNG (getrandom / arc4random / BCryptGenRandom). False
// only if the OS refused, which callers treat as fatal for that operation.
bool randomBytes(void *out, size_t n);

// FNV-1a, 64-bit: a fast non-cryptographic hash (change detection, cache
// file identity). `h` continues an earlier hash; the default starts one.
// The basis is a digit short of FNV's published 14695981039346656037: every
// copy in the tree (and plat's core::fnv1a) has always used this one, and
// names derived from it (cache keys, instance sockets) must stay stable.
inline constexpr uint64_t kFnvOffset = 1469598103934665603ull;
uint64_t                  fnv1a(std::string_view data, uint64_t h = kFnvOffset);

std::string base64(std::string_view data);    // standard alphabet, padded
std::string base64url(std::string_view data); // RFC 4648 §5, no padding
// Either alphabet, padding optional, whitespace skipped. False on junk.
bool        base64Decode(std::string_view in, std::string *out);
std::string hex(std::string_view data); // lower case

template <size_t N>
std::string_view bytes(const std::array<uint8_t, N> &a) {
    return {reinterpret_cast<const char *>(a.data()), N};
}

} // namespace crypto
