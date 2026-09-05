// f4-assets/src/manifest.cpp
//
// Manifest JSON reader/writer. Uses f4::json::Reader/Writer (the project's
// shared zero-deps JSON primitives) so f4-assets stays link-clean.

#include <f4/assets/manifest.hpp>
#include <f4/json/reader.hpp>
#include <f4/json/writer.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace f4::assets {

std::string_view capability_status_to_string(CapabilityStatus s) noexcept {
    switch (s) {
        case CapabilityStatus::present: return "present";
        case CapabilityStatus::none:      return "none";
        case CapabilityStatus::unknown:   return "unknown";
    }
    return "unknown";
}

CapabilityStatus capability_status_from_string(std::string_view s) noexcept {
    if (s == "present") return CapabilityStatus::present;
    if (s == "none")    return CapabilityStatus::none;
    return CapabilityStatus::unknown;
}

const Capability* AssetEntry::find_capability(std::string_view name) const noexcept {
    for (const auto& c : capabilities) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

Capability* AssetEntry::find_capability(std::string_view name) noexcept {
    for (auto& c : capabilities) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

const AssetEntry* Manifest::find(const AssetId& id) const noexcept {
    for (const auto& a : assets) {
        if (a.id == id) return &a;
    }
    return nullptr;
}

AssetEntry* Manifest::find(const AssetId& id) noexcept {
    for (auto& a : assets) {
        if (a.id == id) return &a;
    }
    return nullptr;
}

CapabilityLookup Manifest::capability_for(
    const AssetId& id, std::string_view capability_name) const noexcept {
    CapabilityLookup out;
    const AssetEntry* e = find(id);
    if (!e) return out;
    out.asset_known = true;
    out.cap = e->find_capability(capability_name);
    return out;
}

namespace {

void expect_top_level_marker(f4::json::Reader& r) {
    r.skip_ws();
    r.expect('{');
    r.skip_ws();
    std::string key = r.read_string();
    if (key != "f4") {
        throw std::runtime_error(
            "Manifest: expected top-level 'f4' envelope marker, got '" + key + "'");
    }
    r.expect(':');
    r.skip_ws();
    r.expect('{');
    bool saw_v = false;
    while (!r.consume('}')) {
        r.skip_ws();
        std::string f4key = r.read_string();
        r.expect(':');
        if (f4key == "v") {
            int v = static_cast<int>(r.read_int());
            if (v != kManifestFormatVersion) {
                throw std::runtime_error(
                    "Manifest: format version mismatch — file is v" +
                    std::to_string(v) + ", expected v" +
                    std::to_string(kManifestFormatVersion));
            }
            saw_v = true;
        } else {
            r.skip_value();
        }
        r.skip_ws();
        (void)r.consume(',');
    }
    if (!saw_v) {
        throw std::runtime_error("Manifest: missing 'f4.v' version field");
    }
    r.skip_ws();
    (void)r.consume(',');
}

Capability read_capability(f4::json::Reader& r) {
    Capability c;
    r.expect('{');
    while (!r.consume('}')) {
        r.skip_ws();
        std::string key = r.read_string();
        r.expect(':');
        if (key == "name") {
            c.name = r.read_string();
        } else if (key == "status") {
            c.status = capability_status_from_string(r.read_string());
        } else if (key == "count") {
            c.count = static_cast<int>(r.read_int());
        } else if (key == "detail") {
            c.detail = r.read_string();
        } else {
            r.skip_value();
        }
        r.skip_ws();
        (void)r.consume(',');
    }
    return c;
}

AssetSource read_source(f4::json::Reader& r) {
    AssetSource s;
    r.expect('{');
    while (!r.consume('}')) {
        r.skip_ws();
        std::string key = r.read_string();
        r.expect(':');
        if (key == "path") {
            s.path = r.read_string();
        } else if (key == "role") {
            s.role = r.read_string();
        } else if (key == "sha256") {
            s.sha256 = r.read_string();
        } else {
            r.skip_value();
        }
        r.skip_ws();
        (void)r.consume(',');
    }
    return s;
}

AssetEntry read_asset(f4::json::Reader& r) {
    AssetEntry a;
    r.expect('{');
    while (!r.consume('}')) {
        r.skip_ws();
        std::string key = r.read_string();
        r.expect(':');
        if (key == "id") {
            a.id = parse_asset_id(r.read_string());
        } else if (key == "path") {
            a.path = r.read_string();
        } else if (key == "format_version") {
            a.format_version = static_cast<int>(r.read_int());
        } else if (key == "capabilities") {
            r.skip_ws();
            r.expect('[');
            while (!r.consume(']')) {
                a.capabilities.push_back(read_capability(r));
                r.skip_ws();
                (void)r.consume(',');
            }
        } else if (key == "sources") {
            r.skip_ws();
            r.expect('[');
            while (!r.consume(']')) {
                a.sources.push_back(read_source(r));
                r.skip_ws();
                (void)r.consume(',');
            }
        } else {
            r.skip_value();
        }
        r.skip_ws();
        (void)r.consume(',');
    }
    return a;
}

} // namespace

Manifest read_manifest(std::string_view json) {
    std::string s(json);
    f4::json::Reader r(s);
    Manifest m;

    expect_top_level_marker(r);

    while (!r.consume('}')) {
        r.skip_ws();
        std::string key = r.read_string();
        r.expect(':');
        if (key == "data_dir") {
            m.data_dir = r.read_string();
        } else if (key == "excluded_dirs") {
            r.skip_ws();
            r.expect('[');
            while (!r.consume(']')) {
                m.excluded_dirs.push_back(r.read_string());
                r.skip_ws();
                (void)r.consume(',');
            }
        } else if (key == "assets") {
            r.skip_ws();
            r.expect('[');
            while (!r.consume(']')) {
                m.assets.push_back(read_asset(r));
                r.skip_ws();
                (void)r.consume(',');
            }
        } else {
            r.skip_value();
        }
        r.skip_ws();
        (void)r.consume(',');
    }

    return m;
}

Manifest read_manifest_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("read_manifest_file: cannot open " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return read_manifest(ss.str());
}

namespace {

void write_capability(f4::json::Writer& w, const Capability& c, const char* indent) {
    w.raw(indent);
    w.raw("{\n");
    w.raw(indent);
    w.raw("  ");
    w.string_key("name", c.name);
    w.raw(",\n");
    w.raw(indent);
    w.raw("  \"status\": ");
    w.string(capability_status_to_string(c.status));
    if (c.count.has_value()) {
        w.raw(",\n");
        w.raw(indent);
        w.raw("  ");
        w.number_key("count", *c.count);
    }
    if (c.detail.has_value()) {
        w.raw(",\n");
        w.raw(indent);
        w.raw("  ");
        w.string_key("detail", *c.detail);
    }
    w.raw("\n");
    w.raw(indent);
    w.raw("}");
}

void write_source(f4::json::Writer& w, const AssetSource& s, const char* indent) {
    w.raw(indent);
    w.raw("{\n");
    w.raw(indent);
    w.raw("  ");
    w.string_key("path", s.path);
    w.raw(",\n");
    w.raw(indent);
    w.raw("  ");
    w.string_key("role", s.role);
    w.raw(",\n");
    w.raw(indent);
    w.raw("  ");
    w.string_key("sha256", s.sha256);
    w.raw("\n");
    w.raw(indent);
    w.raw("}");
}

void write_asset(f4::json::Writer& w, const AssetEntry& a, const char* indent) {
    w.raw(indent);
    w.raw("{\n");
    w.raw(indent);
    w.raw("  ");
    w.string_key("id", a.id.to_string());
    w.raw(",\n");
    w.raw(indent);
    w.raw("  ");
    w.string_key("path", a.path);
    w.raw(",\n");
    w.raw(indent);
    w.raw("  ");
    w.number_key("format_version", a.format_version);
    if (!a.capabilities.empty()) {
        w.raw(",\n");
        w.raw(indent);
        w.raw("  \"capabilities\": [\n");
        const std::size_t cn = a.capabilities.size();
        for (std::size_t i = 0; i < cn; ++i) {
            write_capability(w, a.capabilities[i], (std::string(indent) + "    ").c_str());
            if (i + 1 < cn) w.raw(",");
            w.raw("\n");
        }
        w.raw(indent);
        w.raw("  ]");
    }
    if (!a.sources.empty()) {
        w.raw(",\n");
        w.raw(indent);
        w.raw("  \"sources\": [\n");
        const std::size_t sn = a.sources.size();
        for (std::size_t i = 0; i < sn; ++i) {
            write_source(w, a.sources[i], (std::string(indent) + "    ").c_str());
            if (i + 1 < sn) w.raw(",");
            w.raw("\n");
        }
        w.raw(indent);
        w.raw("  ]");
    }
    w.raw("\n");
    w.raw(indent);
    w.raw("}");
}

} // namespace

std::string write_manifest(const Manifest& m) {
    f4::json::Writer w;
    w.raw("{\n");
    w.raw("  \"f4\": {\n");
    w.raw("    ");
    w.number_key("v", kManifestFormatVersion);
    w.raw(",\n");
    w.raw("    ");
    w.string_key("generator", m.generator);
    w.raw("\n");
    w.raw("  },\n");
    w.raw("  ");
    w.string_key("data_dir", m.data_dir);
    if (!m.excluded_dirs.empty()) {
        w.raw(",\n  \"excluded_dirs\": [");
        for (std::size_t i = 0; i < m.excluded_dirs.size(); ++i) {
            if (i) w.raw(", ");
            w.string(m.excluded_dirs[i]);
        }
        w.raw("]");
    }
    w.raw(",\n");
    w.raw("  \"assets\": [\n");
    const std::size_t n = m.assets.size();
    for (std::size_t i = 0; i < n; ++i) {
        write_asset(w, m.assets[i], "    ");
        if (i + 1 < n) w.raw(",");
        w.raw("\n");
    }
    w.raw("  ]\n");
    w.raw("}\n");
    return w.str();
}

void write_manifest_file(const std::string& path, const Manifest& m) {
    const std::string json = write_manifest(m);
    const std::filesystem::path p(path);
    const auto tmp = p.parent_path() / (p.filename().string() + ".tmp");
    {
        std::ofstream f(tmp);
        if (!f) throw std::runtime_error("write_manifest_file: cannot open " + tmp.string());
        f << json;
        if (!f) throw std::runtime_error("write_manifest_file: write failed on " + tmp.string());
    }
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        throw std::runtime_error("write_manifest_file: rename failed: " + ec.message());
    }
}

} // namespace f4::assets
