// f4-assets/include/f4/assets/hash.hpp
//
// Content fingerprints — the runtime half of spec P7 ("existence alone is
// never evidence of freshness"). The manifest records, per asset, the size,
// FNV-1a 64, and SHA-256 of the exported file; the runtime recomputes them
// and reports `stale` on any mismatch.
//
// Both hashes are implemented here (zero deps, ~120 lines total) rather than
// vendoring a crypto library: FNV-1a is the fast change detector, SHA-256 is
// the strong one, and the generator half lives in
// scripts/generate_manifest.py (Python hashlib — the two sides agree on the
// hex encodings, pinned by test_hash.cpp against published vectors AND
// against the committed Data/manifest.json fingerprints).

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace f4::assets {

// FNV-1a 64 — raw value.
[[nodiscard]] std::uint64_t fnv1a_64(std::string_view data) noexcept;

// FNV-1a 64 — 16-char lowercase hex (the manifest's "fnv1a_64" form).
[[nodiscard]] std::string fnv1a_64_hex(std::string_view data) noexcept;

// SHA-256 — 64-char lowercase hex (the manifest's "sha256" form).
[[nodiscard]] std::string sha256_hex(std::string_view data) noexcept;

// SHA-256 of a file's contents. nullopt on I/O error (missing/unreadable).
[[nodiscard]] std::optional<std::string> sha256_file_hex(const std::filesystem::path& p);

// File size in bytes. nullopt on error.
[[nodiscard]] std::optional<std::uintmax_t> file_size_bytes(const std::filesystem::path& p);

} // namespace f4::assets
