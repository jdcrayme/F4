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

    [[nodiscard]] const Capability* find_capability(std::string_view name) const noexcept;
    [[nodiscard]] Capability* find_capability(std::string_view name) noexcept;
};

struct Manifest {
    int format_version = kManifestFormatVersion;
    std::string generator;
    std::string data_dir;
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
