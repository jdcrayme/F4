// f4-assets/include/f4/assets/asset_root.hpp
//
// Asset root resolution — the single point where the runtime decides
// where `Data/` lives on disk. See ASSET_PIPELINE_SPEC §8.

#pragma once

#include <f4/assets/manifest.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace f4::assets {

class AssetRoot {
public:
    AssetRoot() = default;

    static std::optional<AssetRoot> discover();
    static std::optional<AssetRoot> at(std::filesystem::path data_dir);

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const std::filesystem::path& data_dir() const noexcept { return data_dir_; }
    [[nodiscard]] const Manifest& manifest() const noexcept { return manifest_; }

    [[nodiscard]] std::filesystem::path resolve_asset_path(const AssetId& id) const;
    [[nodiscard]] std::filesystem::path resolve_existing(const AssetId& id) const;

private:
    bool valid_ = false;
    std::filesystem::path data_dir_;
    Manifest manifest_;
};

enum class AssetStatus {
    ok,
    missing,
    stale,
    unknown_capability
};

struct RequiredAsset {
    AssetId id;
    std::string required_capability;

    static RequiredAsset model(AssetId id) { return {std::move(id), ""}; }
    static RequiredAsset with_capability(AssetId id, std::string cap) {
        return {std::move(id), std::move(cap)};
    }
};

struct AssetReport {
    AssetId id;
    AssetStatus status = AssetStatus::missing;
    std::string detail;
};

[[nodiscard]] std::vector<AssetReport> check(const AssetRoot& root,
                                              const std::vector<RequiredAsset>& required);

// ── @asset: reference resolution (Task 58) ────────────────────────────────
//
// The consumer-side half of the pipeline: scenario JSONs (and world JSON
// `terrain_file`) carry "@asset:<id>" strings; this is where they become
// concrete paths. Non-@asset: inputs pass through unchanged (the caller
// resolves them against their own base dir); @asset: inputs go through the
// manifest, and a missing id or file is an ERROR (fail loud — the manifest
// is the authority).

struct RefResolution {
    bool ok = false;
    std::filesystem::path path;  // concrete path when ok; empty otherwise
    std::string error;           // human-readable reason when !ok
};

[[nodiscard]] RefResolution resolve_ref(const AssetRoot& root,
                                        std::string_view asset_ref_or_path);

// Hash-verify one entry's file against its recorded fingerprints (size,
// sha256, fnv1a_64 — whichever are recorded). Returns nullopt when the file
// matches (or the entry carries no fingerprints and there is nothing to
// check); returns a human-readable mismatch reason otherwise.
[[nodiscard]] std::optional<std::string> verify_entry_fingerprints(
    const AssetRoot& root, const AssetEntry& entry);

} // namespace f4::assets
