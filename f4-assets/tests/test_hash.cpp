// f4-assets/tests/test_hash.cpp
//
// Task 58 (NO_BINARY_RUNTIME_PLAN Tranche 0e): content fingerprints.
// The hashes are pinned three ways:
//   1. Published vectors (FNV-1a reference values, FIPS 180-4 SHA-256).
//   2. Cross-check against Python (scripts/generate_manifest.py uses
//      hashlib — the same algorithm; the hex encodings must agree).
//   3. The COMMITTED Data/ fingerprints — when the repo's Data/ tree is
//      available, the C++ SHA-256/FNV-1a of the real exported files must
//      reproduce the values recorded in Data/manifest.json. This is the
//      generator↔runtime contract, verified on every test run.

#include <f4/assets/hash.hpp>
#include <f4/assets/manifest.hpp>

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

using namespace f4::assets;

namespace {
namespace fs = std::filesystem;

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

// Locate the repo source dir the way f4-simulation tests do (CMake
// exposes F4_SOURCE_DIR; fall back to relative probe for in-tree runs).
std::optional<fs::path> find_source_dir() {
#ifdef F4_SOURCE_DIR
    if (fs::exists(fs::path(F4_SOURCE_DIR) / "Data" / "manifest.json"))
        return fs::path(F4_SOURCE_DIR);
#endif
    for (fs::path p = fs::current_path(); !p.empty(); p = p.parent_path()) {
        if (fs::exists(p / "Data" / "manifest.json")) return p;
    }
    return std::nullopt;
}

} // namespace

// ── 1. Published vectors ─────────────────────────────────────────────────

TEST(Fnv1a64, ReferenceVectors) {
    // FNV-1a 64 reference values (Landon Curt Noll's test vectors).
    EXPECT_EQ(fnv1a_64_hex(""), "cbf29ce484222325");
    EXPECT_EQ(fnv1a_64_hex("a"), "af63dc4c8601ec8c");
    EXPECT_EQ(fnv1a_64_hex("foobar"), "85944171f73967e8");
}

TEST(Fnv1a64, RawValueMatchesHexForm) {
    const std::uint64_t v = fnv1a_64("hello world");
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(v));
    EXPECT_EQ(fnv1a_64_hex("hello world"), buf);
}

TEST(Sha256, Fips180_4Vectors) {
    // FIPS 180-4 / NIST test vectors.
    EXPECT_EQ(sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb924"
                              "27ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223"
                                 "b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // One 'a' repeated one million times — crosses many 64-byte blocks and
    // exercises the 128-byte padding path.
    const std::string million(1'000'000, 'a');
    EXPECT_EQ(sha256_hex(million), "cdc76e5c9914fb9281a1c7e284d73e67"
                                   "f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, BlockBoundaryInputs) {
    // 55/56/57/63/64/65 bytes: the padding boundary cases (56-byte
    // length field must spill into a second block at exactly 56).
    for (int n : {0, 1, 55, 56, 57, 63, 64, 65, 127, 128, 129}) {
        const std::string input(static_cast<std::size_t>(n), 'x');
        // Sanity only — the digest must be well-formed hex of the right
        // length and stable across recomputation.
        const std::string h1 = sha256_hex(input);
        const std::string h2 = sha256_hex(input);
        EXPECT_EQ(h1.size(), 64u);
        EXPECT_EQ(h1, h2);
    }
}

// ── 2. File helpers ──────────────────────────────────────────────────────

TEST(Sha256File, HashesRealFile) {
    const auto tmp = fs::temp_directory_path() / "f4_hash_test_file.txt";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << "abc";
    }
    const auto h = sha256_file_hex(tmp);
    ASSERT_TRUE(h.has_value());
    EXPECT_EQ(*h, sha256_hex("abc"));
    const auto sz = file_size_bytes(tmp);
    ASSERT_TRUE(sz.has_value());
    EXPECT_EQ(*sz, 3u);
    fs::remove(tmp);
}

TEST(Sha256File, MissingFileIsNullopt) {
    EXPECT_FALSE(sha256_file_hex("/nonexistent/f4/missing.bin").has_value());
    EXPECT_FALSE(file_size_bytes("/nonexistent/f4/missing.bin").has_value());
}

// ── 3. The committed Data/ fingerprints (generator↔runtime contract) ────

TEST(Sha256, ReproducesCommittedManifestFingerprints) {
    const auto src = find_source_dir();
    if (!src) GTEST_SKIP() << "no Data/ tree available (F4_SOURCE_DIR unset)";

    std::ifstream mf(*src / "Data" / "manifest.json");
    ASSERT_TRUE(mf);
    const std::string manifest_json((std::istreambuf_iterator<char>(mf)),
                                    std::istreambuf_iterator<char>());
    // Minimal scan: pull every entry's path + sha256 + fnv1a_64 + size out
    // via the real manifest reader.
    const Manifest m = read_manifest(manifest_json);
    ASSERT_FALSE(m.assets.empty());

    int verified = 0;
    for (const auto& a : m.assets) {
        ASSERT_TRUE(a.has_fingerprints());
        ASSERT_TRUE(a.sha256.has_value());
        const auto p = *src / "Data" / a.path;
        ASSERT_TRUE(fs::exists(p)) << a.path;
        const std::string content = read_file(p);
        EXPECT_EQ(sha256_hex(content), *a.sha256) << a.path;
        if (a.fnv1a_64) {
            EXPECT_EQ(fnv1a_64_hex(content), *a.fnv1a_64) << a.path;
        }
        if (a.size_bytes) {
            EXPECT_EQ(content.size(), *a.size_bytes) << a.path;
        }
        ++verified;
    }
    EXPECT_GE(verified, 30);  // the committed tree has 30+ exported files
}
