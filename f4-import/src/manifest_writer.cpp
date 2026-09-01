// f4-import/src/manifest_writer.cpp

#include <f4/import/manifest_writer.hpp>
#include <f4/assets/manifest.hpp>

#include <cctype>
#include <filesystem>

namespace f4::import {

std::string to_lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

f4::assets::Manifest load_or_create_manifest(const std::filesystem::path& data_dir) {
    namespace fs = std::filesystem;
    f4::assets::Manifest m;
    m.format_version = f4::assets::kManifestFormatVersion;
    m.data_dir = "Data/";
    const auto mp = data_dir / "manifest.json";
    std::error_code ec;
    if (fs::exists(mp, ec)) {
        try {
            m = f4::assets::read_manifest_file(mp.string());
        } catch (const std::exception&) {
            m = f4::assets::Manifest{};
            m.format_version = f4::assets::kManifestFormatVersion;
            m.data_dir = "Data/";
        }
    }
    return m;
}

void upsert_asset(f4::assets::Manifest& m,
                   const f4::assets::AssetId& id,
                   std::string path,
                   int format_version,
                   std::vector<f4::assets::Capability> capabilities,
                   std::vector<f4::assets::AssetSource> sources) {
    f4::assets::AssetEntry* existing = m.find(id);
    if (existing) {
        existing->path = std::move(path);
        existing->format_version = format_version;
        existing->capabilities = std::move(capabilities);
        existing->sources = std::move(sources);
        return;
    }
    f4::assets::AssetEntry e;
    e.id = id;
    e.path = std::move(path);
    e.format_version = format_version;
    e.capabilities = std::move(capabilities);
    e.sources = std::move(sources);
    m.assets.push_back(std::move(e));
}

std::filesystem::path update_manifest_for_asset(
    const std::filesystem::path& data_dir,
    const f4::assets::AssetId& id,
    std::string path,
    int format_version,
    std::vector<f4::assets::Capability> capabilities,
    std::vector<f4::assets::AssetSource> sources,
    const std::string& generator) {
    namespace fs = std::filesystem;
    f4::assets::Manifest m = load_or_create_manifest(data_dir);
    if (m.generator.empty()) m.generator = generator;
    upsert_asset(m, id, std::move(path), format_version,
                  std::move(capabilities), std::move(sources));
    std::error_code ec;
    fs::create_directories(data_dir, ec);
    const auto mp = data_dir / "manifest.json";
    f4::assets::write_manifest_file(mp.string(), m);
    return mp;
}

f4::assets::AssetId campaign_id_from_cam_path(const std::filesystem::path& cam_path) {
    return f4::assets::AssetId{f4::assets::AssetFamily::campaign,
                                to_lower_ascii(cam_path.stem().string())};
}

f4::assets::AssetId theater_id_from_name(const std::string& name) {
    return f4::assets::AssetId{f4::assets::AssetFamily::theater,
                                to_lower_ascii(name)};
}

} // namespace f4::import
