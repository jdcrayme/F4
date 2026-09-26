// f4-simulation/src/harness_shared.hpp
//
// Code shared verbatim by the three combat-chain acceptance harnesses
// (bvr_intercept_harness, wvr_merge_harness, ground_strike_harness).
// The full base-class merge (one CRTP CombatChainHarness owning the
// run-pass skeleton, census walk, and certificate machinery) is the
// follow-up; this header starts with the pieces that are already
// byte-identical.

#pragma once

#include <filesystem>
#include <fstream>
#include <string>

#if defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#endif

#include <f4/json/reader.hpp>

namespace f4::simulation::harness_shared {

/// The M4/M5 pre-flight refusal scan: is this scenario structurally
/// wrong for a combat harness (combat.enabled == false)? Best-effort
/// scan of the raw JSON — any parse problem is IGNORED (run_pass_'s
/// load_scenario() names the real problem), and a non-combat scenario
/// need not carry a valid aircraft list — that is the point of
/// refusing it before any load. Returns true and fills *reason (the
/// QC tools' exit-2 contract prefix: "scenario combat.enabled is
/// false") when the harness must refuse.
inline bool combat_refusal_reason(const std::filesystem::path& scenario_json,
                                  std::string* reason) {
    std::ifstream in(scenario_json);
    if (!in) {
        return false;  // run_pass_'s load_scenario() reports the I/O error
    }
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    try {
        f4::json::Reader r(text);
        r.skip_ws();
        r.expect('{');
        if (r.consume('}')) {
            return false;
        }
        bool combat_enabled = false;  // CombatConfig default
        for (;;) {
            const std::string key = r.read_string();
            r.expect(':');
            if (key == "combat") {
                r.expect('{');
                if (!r.consume('}')) {
                    for (;;) {
                        const std::string ck = r.read_string();
                        r.expect(':');
                        if (ck == "enabled") {
                            combat_enabled = r.read_bool();
                        } else {
                            r.skip_value();
                        }
                        if (r.consume('}')) break;
                        r.expect(',');
                    }
                }
            } else {
                r.skip_value();
            }
            if (r.consume('}')) break;
            r.expect(',');
        }
        if (!combat_enabled) {
            if (reason != nullptr) {
                *reason =
                    "scenario combat.enabled is false — the "
                    "harness refuses to run a non-combat scenario "
                    "(silent success would be the worst failure "
                    "class)";
            }
            return true;
        }
    } catch (const std::exception&) {
        // Malformed or unexpected shape — run_pass_'s
        // load_scenario() names the real problem.
    }
    return false;
}

} // namespace f4::simulation::harness_shared


namespace f4::simulation::harness_shared {

// ===========================================================================
// The determinism certificate's digest, the diary's RSS telemetry, and
// the combat-event kind ints — previously copy-pasted verbatim in all
// four harness translation units (campaign_war, bvr_intercept, wvr_merge,
// ground_strike; each copy's comment said "a third consumer would be the
// moment to extract" — there were four). The MD5 is pinned by the test
// vectors (md5("") / md5("abc")) in the harness test suites.
// ===========================================================================

constexpr int kTrackAcquired    = 0;
constexpr int kTrackDropped     = 1;
constexpr int kRwrLock          = 2;
constexpr int kRwrLaunch        = 3;
constexpr int kMissileLaunched  = 4;
constexpr int kMissileDetonated = 5;
constexpr int kDamageApplied    = 6;
constexpr int kEntityKilled     = 7;
constexpr int kGunFired         = 8;
constexpr int kBombReleased     = 9;
constexpr int kBombImpact       = 10;

/// "target_hit" is the MissileEndCause name for a clean fuze hit (see
/// f4::weapons::missile_end_cause_name). The engagement_summary's
/// shots_hit counts MissileDetonated events with this end_cause.
constexpr const char* kEndCauseTargetHit = "target_hit";

class Md5 {
public:
    void update(const char* data, std::size_t len) {
        total_bits_ += static_cast<std::uint64_t>(len) * 8u;
        while (len > 0) {
            const std::size_t take = std::min(len, 64u - buffer_len_);
            for (std::size_t i = 0; i < take; ++i) {
                buffer_[buffer_len_ + i] =
                    static_cast<std::uint8_t>(data[i]);
            }
            buffer_len_ += take;
            data += take;
            len -= take;
            if (buffer_len_ == 64) {
                process_block(buffer_);
                buffer_len_ = 0;
            }
        }
    }

    std::string hex_digest() {
        const std::uint8_t one = 0x80;
        update_tail(&one, 1);
        const std::uint8_t zero = 0x00;
        while (buffer_len_ != 56) {
            update_tail(&zero, 1);
        }
        std::uint8_t len_bytes[8];
        const auto bits = total_bits_;
        for (int i = 0; i < 8; ++i) {
            len_bytes[i] =
                static_cast<std::uint8_t>((bits >> (8 * i)) & 0xff);
        }
        update_tail(len_bytes, 8);

        std::string out;
        out.reserve(32);
        const std::uint32_t words[4] = {a_, b_, c_, d_};
        for (const auto w : words) {
            for (int i = 0; i < 4; ++i) {
                const auto byte =
                    static_cast<std::uint8_t>((w >> (8 * i)) & 0xff);
                out += "0123456789abcdef"[byte >> 4];
                out += "0123456789abcdef"[byte & 0x0f];
            }
        }
        return out;
    }

private:
    void update_tail(const std::uint8_t* data, std::size_t len) {
        while (len > 0) {
            const std::size_t take = std::min(len, 64u - buffer_len_);
            for (std::size_t i = 0; i < take; ++i) {
                buffer_[buffer_len_ + i] = data[i];
            }
            buffer_len_ += take;
            data += take;
            len -= take;
            if (buffer_len_ == 64) {
                process_block(buffer_);
                buffer_len_ = 0;
            }
        }
    }

    static constexpr std::uint32_t left_rotate(std::uint32_t v, int bits) {
        return (v << bits) | (v >> (32 - bits));
    }

    void process_block(const std::uint8_t* p) {
        static constexpr std::uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
            0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
            0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
            0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
            0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
            0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
            0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
            0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
            0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
        };
        static constexpr int S[64] = {
            7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
            5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
            6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
        };

        std::uint32_t m[16];
        for (int i = 0; i < 16; ++i) {
            m[i] = static_cast<std::uint32_t>(p[4 * i]) |
                   (static_cast<std::uint32_t>(p[4 * i + 1]) << 8) |
                   (static_cast<std::uint32_t>(p[4 * i + 2]) << 16) |
                   (static_cast<std::uint32_t>(p[4 * i + 3]) << 24);
        }

        std::uint32_t aa = a_, bb = b_, cc = c_, dd = d_;
        for (int i = 0; i < 64; ++i) {
            std::uint32_t f;
            int g;
            if (i < 16) {
                f = (bb & cc) | (~bb & dd);
                g = i;
            } else if (i < 32) {
                f = (dd & bb) | (~dd & cc);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = bb ^ cc ^ dd;
                g = (3 * i + 5) % 16;
            } else {
                f = cc ^ (bb | ~dd);
                g = (7 * i) % 16;
            }
            const std::uint32_t tmp = dd;
            dd = cc;
            cc = bb;
            bb = bb + left_rotate(aa + f + K[i] + m[g], S[i]);
            aa = tmp;
        }
        a_ += aa;
        b_ += bb;
        c_ += cc;
        d_ += dd;
    }

    std::uint32_t a_ = 0x67452301;
    std::uint32_t b_ = 0xefcdab89;
    std::uint32_t c_ = 0x98badcfe;
    std::uint32_t d_ = 0x10325476;
    std::uint64_t total_bits_ = 0;
    std::uint8_t buffer_[64]{};
    std::size_t buffer_len_ = 0;
};


std::string md5_hex(const std::string& s) {
    Md5 m;
    m.update(s.data(), s.size());
    return m.hex_digest();
}

long current_rss_kb() {
#if defined(__linux__)
    std::ifstream f("/proc/self/statm");
    unsigned long size = 0, resident = 0;
    if (!(f >> size >> resident)) return 0;
    const long page = sysconf(_SC_PAGESIZE);
    return static_cast<long>(
        (static_cast<unsigned long>(page) / 1024UL) * resident);
#elif defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info),
                  &count) != KERN_SUCCESS) {
        return 0;
    }
    return static_cast<long>(info.resident_size / 1024);
#else
    return 0;
#endif
}

} // namespace f4::simulation::harness_shared
