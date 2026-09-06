// f4-assets/src/hash.cpp
//
// FNV-1a 64 + SHA-256. Standard constructions, kept dependency-free so
// f4-assets stays link-clean (mirrors the f4-json choice). The SHA-256 is
// the straightforward FIPS 180-4 block implementation — performance is a
// non-goal (the largest committed asset is the 11 MB world JSON; hashing it
// is a few tens of ms, and verification runs once at load, not per frame).

#include <f4/assets/hash.hpp>

#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace f4::assets {

std::uint64_t fnv1a_64(std::string_view data) noexcept {
    // 64-bit FNV-1a: offset basis 14695981039346656037, prime 1099511628211.
    std::uint64_t h = 14695981039346656037ULL;
    for (const char c : data) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

namespace {

void append_hex(std::string& out, std::uint64_t v, int nibbles) {
    static constexpr char kDigits[] = "0123456789abcdef";
    const std::size_t base = out.size();
    out.resize(base + static_cast<std::size_t>(nibbles));
    for (int i = nibbles - 1; i >= 0; --i) {
        out[base + static_cast<std::size_t>(i)] = kDigits[v & 0xF];
        v >>= 4;
    }
}

// Reference implementation constants (FIPS 180-4 §4.2.3).
constexpr std::array<std::uint32_t, 64> kSha256K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr std::uint32_t rotr(std::uint32_t x, std::uint32_t n) noexcept {
    return (x >> n) | (x << (32 - n));
}

// Core SHA-256 compression over a 64-byte block, mutating the 8-word state.
void sha256_block(std::array<std::uint32_t, 8>& h, const std::uint8_t* p) {
    std::array<std::uint32_t, 64> w{};
    for (int i = 0; i < 16; ++i) {
        w[static_cast<std::size_t>(i)] =
            (static_cast<std::uint32_t>(p[i * 4]) << 24) |
            (static_cast<std::uint32_t>(p[i * 4 + 1]) << 16) |
            (static_cast<std::uint32_t>(p[i * 4 + 2]) << 8) |
            static_cast<std::uint32_t>(p[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 =
            rotr(w[static_cast<std::size_t>(i - 15)], 7) ^
            rotr(w[static_cast<std::size_t>(i - 15)], 18) ^
            (w[static_cast<std::size_t>(i - 15)] >> 3);
        const std::uint32_t s1 =
            rotr(w[static_cast<std::size_t>(i - 2)], 17) ^
            rotr(w[static_cast<std::size_t>(i - 2)], 19) ^
            (w[static_cast<std::size_t>(i - 2)] >> 10);
        w[static_cast<std::size_t>(i)] =
            w[static_cast<std::size_t>(i - 16)] + s0 +
            w[static_cast<std::size_t>(i - 7)] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = hh + S1 + ch + kSha256K[static_cast<std::size_t>(i)] +
                                 w[static_cast<std::size_t>(i)];
        const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

std::string sha256_digest_hex(const std::uint8_t* data, std::size_t len) {
    std::array<std::uint32_t, 8> h = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };

    // Full blocks.
    std::size_t i = 0;
    for (; i + 64 <= len; i += 64) sha256_block(h, data + i);

    // Padding: remaining bytes, then 0x80, zeros, and the 64-bit big-endian
    // bit length — one or two 64-byte blocks (rem < 56 fits one block).
    std::uint8_t tail[128]{};
    const std::size_t rem = len - i;
    for (std::size_t k = 0; k < rem; ++k) tail[k] = data[i + k];
    tail[rem] = 0x80;
    const std::size_t padded = (rem < 56) ? 64 : 128;
    const std::uint64_t bits = static_cast<std::uint64_t>(len) * 8ULL;
    for (int j = 0; j < 8; ++j) {
        tail[padded - 8 + static_cast<std::size_t>(j)] =
            static_cast<std::uint8_t>((bits >> (56 - 8 * static_cast<std::uint32_t>(j))) & 0xFF);
    }
    sha256_block(h, tail);
    if (padded == 128) sha256_block(h, tail + 64);

    std::string out;
    out.reserve(64);
    for (const std::uint32_t word : h) {
        append_hex(out, (word >> 16) & 0xFFFF, 4);
        append_hex(out, word & 0xFFFF, 4);
    }
    return out;
}

} // namespace

std::string fnv1a_64_hex(std::string_view data) noexcept {
    std::string out;
    out.reserve(16);
    append_hex(out, fnv1a_64(data), 16);
    return out;
}

std::string sha256_hex(std::string_view data) noexcept {
    return sha256_digest_hex(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

std::optional<std::string> sha256_file_hex(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    if (!f && !f.eof()) return std::nullopt;
    const std::string content = ss.str();
    return sha256_digest_hex(reinterpret_cast<const std::uint8_t*>(content.data()),
                             content.size());
}

std::optional<std::uintmax_t> file_size_bytes(const std::filesystem::path& p) {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(p, ec);
    if (ec) return std::nullopt;
    return sz;
}

} // namespace f4::assets
