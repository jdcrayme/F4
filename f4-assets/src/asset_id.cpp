// f4-assets/src/asset_id.cpp

#include <f4/assets/asset_id.hpp>

#include <array>
#include <cctype>
#include <stdexcept>

namespace f4::assets {

namespace {

constexpr std::array<std::string_view, 8> kFamilyNames = {
    "koreaobj", "class", "theater", "campaign", "aircraft", "tileset",
    "simdata", "unknown"
};

} // namespace

std::string_view family_to_string(AssetFamily f) noexcept {
    int i = static_cast<int>(f);
    if (i < 0 || i >= static_cast<int>(kFamilyNames.size())) return "unknown";
    return kFamilyNames[i];
}

AssetFamily family_from_string(std::string_view s) noexcept {
    for (std::size_t i = 0; i < kFamilyNames.size(); ++i) {
        if (kFamilyNames[i] == s) return static_cast<AssetFamily>(i);
    }
    return AssetFamily::unknown;
}

bool is_valid_local_id(std::string_view s) noexcept {
    if (s.empty()) return false;
    for (char c : s) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!(std::islower(uc) || std::isdigit(uc) || c == '.' || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

bool AssetId::valid() const noexcept {
    return family != AssetFamily::unknown && !local_id.empty();
}

std::string AssetId::to_string() const {
    if (!valid()) return {};
    std::string out;
    out.append(family_to_string(family));
    out.push_back(':');
    out.append(local_id);
    return out;
}

AssetId parse_asset_id_or_invalid(std::string_view s) noexcept {
    AssetId id;
    const auto colon = s.find(':');
    if (colon == std::string_view::npos) return id;
    const auto family_str = s.substr(0, colon);
    const auto local = s.substr(colon + 1);
    AssetFamily f = family_from_string(family_str);
    if (f == AssetFamily::unknown) return id;
    if (!is_valid_local_id(local)) return id;
    id.family = f;
    id.local_id = std::string(local);
    return id;
}

AssetId parse_asset_id(std::string_view s) {
    const auto colon = s.find(':');
    if (colon == std::string_view::npos) {
        throw std::invalid_argument(
            "AssetId: missing ':' in '" + std::string(s) + "'");
    }
    const auto family_str = s.substr(0, colon);
    const auto local = s.substr(colon + 1);
    AssetFamily f = family_from_string(family_str);
    if (f == AssetFamily::unknown) {
        throw std::invalid_argument(
            "AssetId: unknown family '" + std::string(family_str) + "'");
    }
    if (!is_valid_local_id(local)) {
        throw std::invalid_argument(
            "AssetId: invalid local-id '" + std::string(local) +
            "' (lowercase [a-z0-9._-] only)");
    }
    return AssetId{f, std::string(local)};
}

AssetId parse_asset_ref(std::string_view s) {
    if (!is_asset_ref(s)) {
        throw std::invalid_argument(
            "AssetId: not an @asset: reference: '" + std::string(s) + "'");
    }
    return parse_asset_id(s.substr(7));
}

std::string to_asset_ref(const AssetId& id) {
    std::string out = "@asset:";
    out.append(id.to_string());
    return out;
}

} // namespace f4::assets
