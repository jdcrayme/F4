// f4-assets/include/f4/assets/asset_id.hpp
//
// Asset identity — the stable logical name every consumable artifact in the
// F4 runtime is addressed by.
//
// Asset IDs are the contract between importer and runtime (see
// Docs/ASSET_PIPELINE_SPEC.md §5). The grammar is:
//
//     <family>:<local-id>
//
// `family` is one of the registered families (koreaobj / class / theater /
// campaign / aircraft / tileset). `local-id` is lowercase `[a-z0-9._-]`,
// no spaces. The zero-padded numeric form for koreaobj (`koreaobj:00042`)
// preserves sort order.
//
// References inside JSON documents (world JSON `terrain_file`, scenario
// paths) use the prefixed form `@asset:<id>` to distinguish an asset ID
// reference from a bare filename. The reader (resolve_asset_ref) accepts
// both forms so legacy JSON keeps working during the migration.

#pragma once

#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace f4::assets {

enum class AssetFamily {
    koreaobj,
    class_,
    theater,
    campaign,
    aircraft,
    tileset,
    unknown
};

struct AssetId {
    AssetFamily family = AssetFamily::unknown;
    std::string local_id;

    AssetId() = default;
    AssetId(AssetFamily f, std::string l) : family(f), local_id(std::move(l)) {}

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::string to_string() const;

    bool operator==(const AssetId& o) const noexcept {
        return family == o.family && local_id == o.local_id;
    }
    bool operator!=(const AssetId& o) const noexcept { return !(*this == o); }
    bool operator<(const AssetId& o) const noexcept {
        if (family != o.family) return family < o.family;
        return local_id < o.local_id;
    }
};

[[nodiscard]] std::string_view family_to_string(AssetFamily f) noexcept;
[[nodiscard]] AssetFamily family_from_string(std::string_view s) noexcept;

[[nodiscard]] AssetId parse_asset_id(std::string_view s);
[[nodiscard]] AssetId parse_asset_id_or_invalid(std::string_view s) noexcept;

[[nodiscard]] inline bool is_asset_ref(std::string_view s) noexcept {
    return s.substr(0, 7) == "@asset:";
}

[[nodiscard]] AssetId parse_asset_ref(std::string_view s);
[[nodiscard]] std::string to_asset_ref(const AssetId& id);

[[nodiscard]] bool is_valid_local_id(std::string_view s) noexcept;

inline std::ostream& operator<<(std::ostream& os, const AssetId& id) {
    return os << id.to_string();
}

} // namespace f4::assets

namespace std {

template <>
struct hash<f4::assets::AssetId> {
    std::size_t operator()(const f4::assets::AssetId& id) const noexcept {
        std::size_t h1 = std::hash<int>{}(static_cast<int>(id.family));
        std::size_t h2 = std::hash<std::string>{}(id.local_id);
        return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};

} // namespace std
