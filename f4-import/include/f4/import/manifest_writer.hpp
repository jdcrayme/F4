// f4-import/include/f4/import/manifest_writer.hpp
//
// Helpers used by the importers (cam2json, terrain2json, future f4import
// subcommands) to write manifest entries as a side effect of a successful
// conversion. See ASSET_PIPELINE_SPEC §7.

#pragma once

#include <f4/assets/manifest.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace f4::import {

[[nodiscard]] f4::assets::Manifest load_or_create_manifest(
    const std::filesystem::path& data_dir);

void upsert_asset(f4::assets::Manifest& m,
                   const f4::assets::AssetId& id,
                   std::string path,
                   int format_version,
                   std::vector<f4::assets::Capability> capabilities,
                   std::vector<f4::assets::AssetSource> sources);

[[nodiscard]] std::filesystem::path update_manifest_for_asset(
    const std::filesystem::path& data_dir,
    const f4::assets::AssetId& id,
    std::string path,
    int format_version,
    std::vector<f4::assets::Capability> capabilities,
    std::vector<f4::assets::AssetSource> sources,
    const std::string& generator);

[[nodiscard]] f4::assets::AssetId campaign_id_from_cam_path(
    const std::filesystem::path& cam_path);

[[nodiscard]] f4::assets::AssetId theater_id_from_name(const std::string& name);

[[nodiscard]] std::string to_lower_ascii(std::string_view s);

} // namespace f4::import
