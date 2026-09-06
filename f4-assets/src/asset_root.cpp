// f4-assets/src/asset_root.cpp

#include <f4/assets/asset_root.hpp>

#include <f4/assets/asset_id.hpp>
#include <f4/assets/hash.hpp>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>

namespace f4::assets {

namespace {

std::optional<std::filesystem::path> try_dir(const std::filesystem::path& p) {
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec)) {
        return std::filesystem::absolute(p, ec);
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> find_data_dir(
    const std::optional<std::filesystem::path>& cli_override) {
    if (cli_override) {
        if (auto d = try_dir(*cli_override)) return d;
    }
    if (const char* env = std::getenv("F4_DATA_DIR")) {
        if (env[0] != '\0') {
            if (auto d = try_dir(std::filesystem::path(env))) return d;
        }
    }
    {
        std::error_code ec;
        std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (ec) exe = std::filesystem::current_path(ec);
        if (!ec) {
            auto p = exe.parent_path() / "Data";
            if (auto d = try_dir(p)) return d;
        }
    }
    {
        std::error_code ec;
        auto p = std::filesystem::current_path(ec) / "Data";
        if (!ec) {
            if (auto d = try_dir(p)) return d;
        }
    }
    return std::nullopt;
}

} // namespace

std::optional<AssetRoot> AssetRoot::discover() {
    auto dir = find_data_dir(std::nullopt);
    if (!dir) return std::nullopt;
    return at(*dir);
}

std::optional<AssetRoot> AssetRoot::at(std::filesystem::path data_dir) {
    AssetRoot r;
    if (!try_dir(data_dir)) return std::nullopt;
    r.data_dir_ = std::filesystem::absolute(data_dir);
    r.valid_ = true;
    const auto manifest_path = r.data_dir_ / "manifest.json";
    std::error_code ec;
    if (std::filesystem::exists(manifest_path, ec)) {
        try {
            r.manifest_ = read_manifest_file(manifest_path.string());
        } catch (const std::exception& e) {
            std::cerr << "AssetRoot: manifest parse failed at "
                      << manifest_path << ": " << e.what() << "\n";
            r.valid_ = false;
            return std::nullopt;
        }
    }
    return r;
}

std::filesystem::path AssetRoot::resolve_asset_path(const AssetId& id) const {
    const AssetEntry* e = manifest_.find(id);
    if (!e) return {};
    return data_dir_ / e->path;
}

std::filesystem::path AssetRoot::resolve_existing(const AssetId& id) const {
    const AssetEntry* e = manifest_.find(id);
    if (!e) return {};
    auto p = data_dir_ / e->path;
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) return p;
    return {};
}

std::vector<AssetReport> check(const AssetRoot& root,
                                const std::vector<RequiredAsset>& required) {
    std::vector<AssetReport> out;
    out.reserve(required.size());
    for (const auto& req : required) {
        AssetReport r;
        r.id = req.id;
        // Optimistic default: ok unless a check below sets otherwise.
        r.status = AssetStatus::ok;
        const AssetEntry* e = root.manifest().find(req.id);
        if (!e) {
            r.status = AssetStatus::missing;
            r.detail = "not in manifest";
            out.push_back(std::move(r));
            continue;
        }
        if (!req.required_capability.empty()) {
            CapabilityLookup cl = root.manifest().capability_for(req.id, req.required_capability);
            if (!cl.asset_known) {
                r.status = AssetStatus::missing;
                r.detail = "not in manifest";
            } else if (!cl.cap) {
                r.status = AssetStatus::unknown_capability;
                r.detail = "capability '" + req.required_capability + "' not declared on asset";
            } else if (cl.cap->is_unknown()) {
                r.status = AssetStatus::unknown_capability;
                r.detail = "capability '" + req.required_capability + "' is unknown (run f4import to reimport)";
            } else if (cl.cap->is_none()) {
                r.status = AssetStatus::missing;
                r.detail = "capability '" + req.required_capability + "' declared none";
            } else {
                // present — fall through to file-existence check
            }
        }
        if (r.status != AssetStatus::ok) {
            out.push_back(std::move(r));
            continue;
        }
        auto p = root.resolve_asset_path(req.id);
        if (p.empty()) {
            r.status = AssetStatus::missing;
            r.detail = "manifest entry has empty path";
        } else {
            std::error_code ec;
            if (std::filesystem::exists(p, ec)) {
                // File exists — now the P7 freshness check (Task 58): any
                // recorded fingerprint that no longer matches the file on
                // disk reports `stale`. Existence alone is never evidence
                // of freshness.
                const AssetEntry* entry = root.manifest().find(req.id);
                if (auto mismatch = verify_entry_fingerprints(root, *entry)) {
                    r.status = AssetStatus::stale;
                    r.detail = *mismatch;
                } else {
                    r.status = AssetStatus::ok;
                    r.detail.clear();
                }
            } else {
                r.status = AssetStatus::missing;
                r.detail = "file missing on disk: " + p.string();
            }
        }
        out.push_back(std::move(r));
    }
    return out;
}

std::optional<std::string> verify_entry_fingerprints(const AssetRoot& root,
                                                     const AssetEntry& entry) {
    if (!entry.has_fingerprints()) return std::nullopt;  // nothing recorded to check
    const auto p = root.data_dir() / entry.path;
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return "file missing on disk: " + p.string();

    if (entry.size_bytes.has_value()) {
        const auto sz = file_size_bytes(p);
        if (!sz) return "cannot stat file: " + p.string();
        if (*sz != *entry.size_bytes) {
            return "size mismatch: manifest says " + std::to_string(*entry.size_bytes) +
                   " bytes, disk has " + std::to_string(*sz) + " (" + p.string() + ")";
        }
    }
    if (entry.fnv1a_64.has_value() || entry.sha256.has_value()) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return "cannot open file: " + p.string();
        std::ostringstream ss;
        ss << f.rdbuf();
        const std::string content = ss.str();
        if (entry.fnv1a_64.has_value() &&
            *entry.fnv1a_64 != fnv1a_64_hex(content)) {
            return "fnv1a_64 mismatch: manifest says " + *entry.fnv1a_64 +
                   ", disk hashes to " + fnv1a_64_hex(content) + " (" + p.string() + ")";
        }
        if (entry.sha256.has_value() && *entry.sha256 != sha256_hex(content)) {
            return "sha256 mismatch: manifest says " + *entry.sha256 +
                   ", disk hashes to " + sha256_hex(content) + " (" + p.string() + ")";
        }
    }
    return std::nullopt;
}

RefResolution resolve_ref(const AssetRoot& root, std::string_view asset_ref_or_path) {
    RefResolution out;
    const std::string s(asset_ref_or_path);
    if (!is_asset_ref(s)) {
        // Not a reference — pass through; the caller resolves it against
        // its own base directory.
        out.ok = true;
        out.path = std::filesystem::path(s);
        return out;
    }
    if (!root.valid()) {
        out.error = "asset root invalid — cannot resolve " + s;
        return out;
    }
    AssetId id;
    try {
        id = parse_asset_ref(s);
    } catch (const std::exception& e) {
        out.error = e.what();
        return out;
    }
    const AssetEntry* e = root.manifest().find(id);
    if (!e) {
        out.error = "id '" + id.to_string() + "' is not in the manifest (" +
                    root.data_dir().string() + "/manifest.json)";
        return out;
    }
    const auto p = root.data_dir() / e->path;
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) {
        out.error = "file missing on disk for " + id.to_string() + ": " + p.string();
        return out;
    }
    out.ok = true;
    out.path = p;
    return out;
}

} // namespace f4::assets
