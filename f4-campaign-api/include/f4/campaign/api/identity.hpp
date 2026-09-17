// f4-campaign-api/include/f4/campaign/api/identity.hpp
//
// The session identity fingerprint (CAMP_HOST_PLAN.md §5): the value a
// replay asserts against. Determinism across the contract boundary is the
// statement "the same (save, seed, command journal) produces the same
// identity" — the same discipline the C5 harness pins with its ledger
// MD5, exposed to every host.
//
// The fingerprint is deliberately three fields, not a kitchen sink:
//   - protocol_version: the wire dialect (additive-field-only evolution).
//   - campaign_time_s:  the war's absolute clock at the assertion.
//   - ledger_fnv:       FNV-1a 64 over the byte-stable ledger JSON — the
//                       books ARE the war's result; a stable hash over
//                       their canonical bytes is a stable identity.
//
// HOST-2 may extend this struct with the engine's own ledger MD5 when the
// engine exposes it; additive fields only (the protocol's versioning
// rule, CAMP_HOST_PLAN.md §11).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace f4::campaign::api {

// FNV-1a 64 — the dependency-free byte hash. Chosen over pulling f4-assets'
// SHA-256 so this header-only contract library keeps its "f4-json + std
// only" dependency rule; the hash identifies, it does not authenticate.
[[nodiscard]] constexpr std::uint64_t
fnv1a64(std::string_view bytes,
        std::uint64_t seed = 14695981039346656037ULL) noexcept {
    std::uint64_t h = seed;
    for (const char c : bytes) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 1099511628211ULL;
    }
    return h;
}

// Lowercase 16-hex-digit form of an FNV-1a 64 digest (the canonical wire
// spelling; goldens pin this string, not its numeric value).
[[nodiscard]] inline std::string to_hex16(std::uint64_t v) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] =
            kDigits[static_cast<std::size_t>(v & 0xFULL)];
        v >>= 4;
    }
    return out;
}

struct IdentityFingerprint {
    std::uint32_t protocol_version{1};
    /// The war's absolute clock (the save's epoch + the ladder's clock) —
    /// the same value the session's `time` query reports.
    std::int64_t campaign_time_s{0};
    /// fnv1a64 over the byte-stable ledger JSON, hex (see to_hex16).
    std::string ledger_fnv;
};

} // namespace f4::campaign::api
