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

} // namespace f4::assets
