// f4-assets/include/f4/assets/manifest.hpp
//
// Manifest schema v1 — the contract between importer and runtime.
// See Docs/ASSET_PIPELINE_SPEC.md §7 and §8.

#pragma once

#include <f4/assets/asset_id.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace f4::assets {

inline constexpr int kManifestFormatVersion = 1;

enum class CapabilityStatus : std::uint8_t {
    present,
    none,
    unknown
};

struct Capability {
    std::string name;
    CapabilityStatus status = CapabilityStatus::unknown;
    std::optional<int> count;
    std::optional<std::string> detail;

    [[nodiscard]] bool is_present() const noexcept { return status == CapabilityStatus::present; }
    [[nodiscard]] bool is_none() const noexcept { return status == CapabilityStatus::none; }
    [[nodiscard]] bool is_unknown() const noexcept { return status == CapabilityStatus::unknown; }
};

struct AssetSource {
    std::string path;
    std::string role;
    std::string sha256;
};

struct CapabilityLookup {
    const Capability* cap = nullptr;
    bool asset_known = false;
};

struct AssetEntry {
    AssetId id;
    std::string path;
    int format_version = 1;
    std::vector<Capability> capabilities;
    std::vector<AssetSource> sources;

    // Content fingerprints of the exported file at `path` (Task 58 — the
    // runtime half of spec P7 staleness detection). The legacy
    // "fingerprint" manifests (generate_manifest.py ≤ 0a) carried these at
    // entry level with no id; the reader accepts both shapes. Absent
    // fingerprints = not verifiable (check() reports ok, never stale).
    std::optional<std::uintmax_t> size_bytes;
    std::optional<std::string> sha256;    // 64 hex chars
    std::optional<std::string> fnv1a_64;  // 16 hex chars

    [[nodiscard]] bool has_fingerprints() const noexcept {
        return size_bytes.has_value() || sha256.has_value() || fnv1a_64.has_value();
    }

    [[nodiscard]] const Capability* find_capability(std::string_view name) const noexcept;
    [[nodiscard]] Capability* find_capability(std::string_view name) noexcept;
};

struct Manifest {
    int format_version = kManifestFormatVersion;
    std::string generator;
    std::string data_dir;
    // Provenance context from the generator (Task 58). `theater` names the
    // theater domain (e.g. "korea") and `save` the campaign save the world
    // was exported from (e.g. "save1") — the campaign:<save> id derivation
    // for World/*.world.json entries needs it. Empty = absent.
    std::string theater;
    std::string save;
    // Top-level directories under data_dir that are intentionally NOT
    // listed in "assets" (e.g. "Models" while it is a local-only,
    // gitignored export). The doctor's unlisted-file scan (D8) skips
    // them; everything else must be listed or it is flagged.
    std::vector<std::string> excluded_dirs;
    std::vector<AssetEntry> assets;

    [[nodiscard]] const AssetEntry* find(const AssetId& id) const noexcept;
    [[nodiscard]] AssetEntry* find(const AssetId& id) noexcept;

    [[nodiscard]] CapabilityLookup capability_for(
        const AssetId& id, std::string_view capability_name) const noexcept;
};

[[nodiscard]] Manifest read_manifest(std::string_view json);
[[nodiscard]] std::string write_manifest(const Manifest& m);
[[nodiscard]] Manifest read_manifest_file(const std::string& path);
void write_manifest_file(const std::string& path, const Manifest& m);

[[nodiscard]] std::string_view capability_status_to_string(CapabilityStatus s) noexcept;
[[nodiscard]] CapabilityStatus capability_status_from_string(std::string_view s) noexcept;

} // namespace f4::assets
