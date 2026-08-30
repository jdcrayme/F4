// f4-terrain/src/file_util.hpp
//
// Internal helpers shared by the theater data decoders (post_level,
// far_tile_db, near_tile_db). Not part of the public interface.

#pragma once

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace f4::terrain::detail {

/// Case-insensitive file lookup inside `dir`. Falcon installs ship mixed
/// case ("FArtILES.PAL", "texture.zip", "THEATER.MAP"); Windows resolves
/// case-insensitively but the code must also work on case-sensitive
/// filesystems. Tries the exact name first, then any entry whose
/// lowercased name matches. Returns an empty path when not found.
inline std::filesystem::path find_file_ci(const std::filesystem::path& dir,
                                          const std::string& name) {
    std::error_code ec;
    const std::filesystem::path direct = dir / name;
    if (std::filesystem::exists(direct, ec)) return direct;

    std::string want;
    want.reserve(name.size());
    for (char c : name) want.push_back(static_cast<char>(
        ::tolower(static_cast<unsigned char>(c))));

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string have = entry.path().filename().string();
        for (char& c : have) c = static_cast<char>(
            ::tolower(static_cast<unsigned char>(c)));
        if (have == want) return entry.path();
    }
    return {};
}

} // namespace f4::terrain::detail
