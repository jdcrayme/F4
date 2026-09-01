// f4-install/src/file_finder.cpp
//
// Canonical case-insensitive file/subdir finders. See file_finder.hpp
// for the design rationale (Stage 2 of the asset pipeline folds the
// duplicated finders here).

#include <f4/install/file_finder.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace f4::install {

namespace {

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

} // namespace

std::filesystem::path find_file_ci(const std::filesystem::path& dir,
                                     std::string_view name) {
    std::error_code ec;
    // Fast path: exact name (works on case-insensitive filesystems and
    // when the caller already knows the canonical case).
    const std::filesystem::path direct = dir / name;
    if (std::filesystem::exists(direct, ec)) {
        // On case-insensitive filesystems (NTFS, FAT, default macOS
        // APFS), exists() returns true even when the on-disk filename
        // case differs. weakly_canonical() resolves the actual on-disk
        // name so callers get the real path. Fall back to the
        // constructed path only if canonicalization fails.
        auto real = std::filesystem::weakly_canonical(direct, ec);
        if (!ec) return real;
        return direct;
    }
    if (!std::filesystem::is_directory(dir, ec)) return {};

    const std::string want = to_lower(name);
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        if (to_lower(entry.path().filename().string()) == want) {
            return entry.path();
        }
    }
    return {};
}

std::filesystem::path find_file_ci_with_variants(
    const std::filesystem::path& dir,
    const std::vector<std::string>& names) {
    for (const auto& n : names) {
        auto p = find_file_ci(dir, n);
        if (!p.empty()) return p;
    }
    return {};
}

std::filesystem::path find_subdir_ci(const std::filesystem::path& dir,
                                        std::string_view name) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return {};
    const std::string want = to_lower(name);
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_directory(ec)) continue;
        if (to_lower(entry.path().filename().string()) == want) {
            return entry.path();
        }
    }
    return {};
}

std::filesystem::path find_file_ci_in_dirs(
    const std::vector<std::filesystem::path>& dirs,
    std::string_view name) {
    for (const auto& d : dirs) {
        auto p = find_file_ci(d, name);
        if (!p.empty()) return p;
    }
    return {};
}

std::filesystem::path find_file_ci_in_dirs_with_variants(
    const std::vector<std::filesystem::path>& dirs,
    const std::vector<std::string>& names) {
    for (const auto& d : dirs) {
        auto p = find_file_ci_with_variants(d, names);
        if (!p.empty()) return p;
    }
    return {};
}

std::filesystem::path find_file_by_extension_ci(
    const std::filesystem::path& base_path,
    std::string_view ext) {
    namespace fs = std::filesystem;
    std::error_code ec;

    // 1. base_path + "." + ext
    {
        auto p = base_path;
        p += ".";
        p += ext;
        if (fs::exists(p, ec)) {
            auto real = fs::weakly_canonical(p, ec);
            if (!ec) return real;
            return p;
        }
    }
    // 2. base_path verbatim (no extension — caller may have included it)
    if (fs::exists(base_path, ec)) {
        auto real = fs::weakly_canonical(base_path, ec);
        if (!ec) return real;
        return base_path;
    }
    // 3. Case-insensitive scan of the parent directory for <stem>.<ext>.
    const auto parent = base_path.parent_path().empty()
        ? fs::current_path(ec)
        : base_path.parent_path();
    const auto stem = base_path.filename().string();
    if (stem.empty() || parent.empty() || !fs::exists(parent, ec)) return {};

    const std::string stem_lower = to_lower(stem);
    const std::string ext_lower = to_lower(ext);
    const std::string target = stem_lower + "." + ext_lower;

    for (const auto& entry : fs::directory_iterator(parent, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        if (to_lower(entry.path().filename().string()) == target) {
            return entry.path();
        }
    }
    return {};
}

} // namespace f4::install
