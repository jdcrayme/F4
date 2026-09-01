// f4-import/src/doctor.cpp
//
// doctor — the Data/ tree + manifest validator. Implements D1-D9 from
// ASSET_PIPELINE_SPEC §9.2.
//
// D1, D3, D5, D6 are deferred to later asset-pipeline stages.

#include <f4/import/doctor.hpp>
#include <f4/assets/manifest.hpp>
#include <f4/json/reader.hpp>
#include <f4/gltf/gltf_loader.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>

namespace f4::import {

namespace {

Severity severity_for(const std::string& check_id) {
    if (check_id == "D1" || check_id == "D2" || check_id == "D3" ||
        check_id == "D4" || check_id == "D5" || check_id == "D8" ||
        check_id == "D9") return Severity::error;
    if (check_id == "D6" || check_id == "D8-case") return Severity::warning;
    return Severity::info;  // D7
}

void add_finding(DoctorReport& out, const std::string& check_id,
                  const std::string& asset_id, const std::string& message) {
    DoctorFinding f;
    f.check_id = check_id;
    f.severity = severity_for(check_id);
    f.asset_id = asset_id;
    f.message = message;
    out.findings.push_back(std::move(f));
    switch (f.severity) {
        case Severity::error:   ++out.errors;   break;
        case Severity::warning: ++out.warnings; break;
        case Severity::info:     ++out.infos;   break;
    }
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

// D1: Every koreaobj asset in the manifest has a .gltf file on disk.
// (Stage 3 — visual_bindings.json with used_by_classes is deferred until
// the class table is converted; this Stage 3 implementation checks the
// simpler "the glTF file exists and is loadable" invariant.)
void check_d1_visual_bindings(const std::filesystem::path& data_dir,
                                const f4::assets::Manifest& manifest,
                                DoctorReport& out) {
    for (const auto& a : manifest.assets) {
        if (a.id.family != f4::assets::AssetFamily::koreaobj) continue;
        auto p = data_dir / a.path;
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) {
            add_finding(out, "D1", a.id.to_string(),
                "koreaobj asset .gltf file missing on disk: " + a.path);
            continue;
        }
        // Try loading the glTF to verify it's well-formed.
        try {
            f4::gltf::GltfDocument doc;
            doc.load(p);
            if (doc.meshes.empty()) {
                add_finding(out, "D1", a.id.to_string(),
                    "koreaobj .gltf has no meshes: " + a.path);
            }
        } catch (const std::exception& e) {
            add_finding(out, "D1", a.id.to_string(),
                std::string("koreaobj .gltf failed to load: ") + e.what());
        }
    }
}

void check_d3_class_table_bindings(const std::filesystem::path&,
                                     const f4::assets::Manifest&,
                                     DoctorReport&) {}

// D5: Node tags conform to the §6 grammar; kind-specific extras present.
// Stage 3 implementation: walk every .gltf file in the Data/ tree,
// check every node name against the <kind>:<id>[.<instance>] grammar,
// and verify that nodes with f4 extras have a matching kind + id.
void check_d5_node_tags(const std::filesystem::path& data_dir,
                         const f4::assets::Manifest& manifest,
                         DoctorReport& out) {
    // Scan Models/ for .gltf files.
    auto models_dir = data_dir / "Models";
    std::error_code ec;
    if (!std::filesystem::is_directory(models_dir, ec)) return;

    for (auto it = std::filesystem::recursive_directory_iterator(models_dir, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (ec) break;
        if (!it->is_regular_file()) continue;
        if (it->path().extension() != ".gltf") continue;

        f4::gltf::GltfDocument doc;
        try {
            doc.load(*it);
        } catch (const std::exception&) {
            // D1 catches load failures; D5 only checks tags on
            // successfully-loaded files.
            continue;
        }

        std::string rel = std::filesystem::relative(*it, data_dir, ec).string();

        for (const auto& node : doc.nodes) {
            if (node.name.empty()) continue;

            // Check the node name against the §6 grammar.
            std::string kind, id;
            if (!f4::gltf::parse_f4_node_name(node.name, kind, id)) {
                // Not a tagged node — that's fine (ordinary scene
                // content like "root" is not tagged).
                continue;
            }

            // The node has a reserved-kind name. Verify it has f4 extras
            // with a matching kind.
            if (!node.has_f4) {
                add_finding(out, "D5", "",
                    "node '" + node.name + "' in " + rel +
                    " has a reserved-kind name but no f4 extras");
                continue;
            }

            if (node.f4.kind != kind) {
                add_finding(out, "D5", "",
                    "node '" + node.name + "' in " + rel +
                    " has f4 kind '" + node.f4.kind +
                    "' but name says '" + kind + "'");
            }

            // Verify DOF nodes have min/max/mult (the §6.2 spec).
            if (kind == "dof") {
                if (!node.f4.dof_min.has_value() || !node.f4.dof_max.has_value()) {
                    add_finding(out, "D5", "",
                        "dof node '" + node.name + "' in " + rel +
                        " missing min/max extras");
                }
            }

            // Verify LOD nodes have level.
            if (kind == "lod") {
                if (!node.f4.lod_level.has_value()) {
                    add_finding(out, "D5", "",
                        "lod node '" + node.name + "' in " + rel +
                        " missing level extra");
                }
            }
        }
    }
}

void check_d6_vocab(const std::filesystem::path&,
                     const f4::assets::Manifest&,
                     DoctorReport&) {}

void check_d2_world_json_refs(const std::filesystem::path& data_dir,
                                const f4::assets::Manifest& manifest,
                                DoctorReport& out) {
    const auto world_dir = data_dir / "World";
    std::error_code ec;
    if (!std::filesystem::is_directory(world_dir, ec)) return;
    for (auto& entry : std::filesystem::directory_iterator(world_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        auto p = entry.path();
        if (p.extension() != ".json") continue;
        std::ifstream f(p);
        if (!f) continue;
        std::ostringstream ss; ss << f.rdbuf();
        std::string contents = ss.str();
        try {
            f4::json::Reader r(contents);
            r.skip_ws();
            r.expect('{');
            while (!r.consume('}')) {
                r.skip_ws();
                std::string key = r.read_string();
                r.expect(':');
                if (key == "terrain_file") {
                    std::string val = r.read_string();
                    if (f4::assets::is_asset_ref(val)) {
                        try {
                            f4::assets::AssetId id = f4::assets::parse_asset_ref(val);
                            if (!manifest.find(id)) {
                                add_finding(out, "D2", id.to_string(),
                                    "world JSON " + p.filename().string() +
                                    " references " + val +
                                    " which is not in the manifest");
                            }
                        } catch (const std::exception& e) {
                            add_finding(out, "D2", "",
                                "world JSON " + p.filename().string() +
                                " has malformed asset ref: " + e.what());
                        }
                    }
                    break;
                } else {
                    r.skip_value();
                }
                r.skip_ws();
                (void)r.consume(',');
            }
        } catch (const std::exception&) {}
    }
}

void check_d4_capability_sources(const std::filesystem::path&,
                                   const f4::assets::Manifest& manifest,
                                   DoctorReport& out) {
    for (const auto& a : manifest.assets) {
        for (const auto& c : a.capabilities) {
            if (c.status == f4::assets::CapabilityStatus::present ||
                c.status == f4::assets::CapabilityStatus::none) {
                if (a.sources.empty()) {
                    add_finding(out, "D4", a.id.to_string(),
                        "capability '" + c.name + "' is " +
                        std::string(f4::assets::capability_status_to_string(c.status)) +
                        " but the asset lists no sources — the importer never consulted the authoritative source");
                }
            }
        }
    }
}

void check_d7_orphans(const std::filesystem::path& data_dir,
                       const f4::assets::Manifest& manifest,
                       DoctorReport& out) {
    std::set<f4::assets::AssetId> referenced_theaters;
    const auto world_dir = data_dir / "World";
    std::error_code ec;
    if (std::filesystem::is_directory(world_dir, ec)) {
        for (auto& entry : std::filesystem::directory_iterator(world_dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            auto p = entry.path();
            if (p.extension() != ".json") continue;
            std::ifstream f(p);
            if (!f) continue;
            std::ostringstream ss; ss << f.rdbuf();
            std::string contents = ss.str();
            try {
                f4::json::Reader r(contents);
                r.skip_ws();
                r.expect('{');
                while (!r.consume('}')) {
                    r.skip_ws();
                    std::string key = r.read_string();
                    r.expect(':');
                    if (key == "terrain_file") {
                        std::string val = r.read_string();
                        if (f4::assets::is_asset_ref(val)) {
                            try {
                                referenced_theaters.insert(f4::assets::parse_asset_ref(val));
                            } catch (...) {}
                        }
                        break;
                    } else {
                        r.skip_value();
                    }
                    r.skip_ws();
                    (void)r.consume(',');
                }
            } catch (...) {}
        }
    }
    for (const auto& a : manifest.assets) {
        if (a.id.family != f4::assets::AssetFamily::theater) continue;
        if (referenced_theaters.count(a.id) == 0) {
            add_finding(out, "D7", a.id.to_string(),
                "theater not referenced by any world JSON (orphan — may become referenced)");
        }
    }
}

void check_d8_id_and_files(const std::filesystem::path& data_dir,
                            const f4::assets::Manifest& manifest,
                            DoctorReport& out) {
    std::unordered_set<std::string> seen_lower;
    for (const auto& a : manifest.assets) {
        std::string key = to_lower(a.id.to_string());
        if (seen_lower.count(key)) {
            add_finding(out, "D8", a.id.to_string(),
                "case-collision: another asset lowercases to '" + key + "'");
        }
        seen_lower.insert(key);
    }
    for (const auto& a : manifest.assets) {
        if (a.path.empty()) {
            add_finding(out, "D8", a.id.to_string(), "manifest entry has empty path");
            continue;
        }
        auto p = data_dir / a.path;
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) {
            add_finding(out, "D8", a.id.to_string(),
                "manifest references '" + a.path + "' but file does not exist");
        }
    }
    std::set<std::string> manifest_paths;
    for (const auto& a : manifest.assets) {
        std::string pp = a.path;
        for (char& c : pp) if (c == '\\') c = '/';
        manifest_paths.insert(to_lower(pp));
    }
    // Build a set of .gltf stems that ARE in the manifest. A .bin file
    // with the same stem as a listed .gltf is the binary companion of
    // that .gltf — it's not a separate asset, it's part of the .gltf's
    // external buffer. D8 should not flag it as unlisted.
    std::set<std::string> listed_gltf_stems;
    for (const auto& a : manifest.assets) {
        if (a.path.size() > 5 && a.path.substr(a.path.size() - 5) == ".gltf") {
            std::string stem = a.path.substr(0, a.path.size() - 5);
            for (char& c : stem) if (c == '\\') c = '/';
            listed_gltf_stems.insert(to_lower(stem));
        }
    }
    static const char* kScanSubdirs[] = {"Models", "Theater", "World", "Classes"};
    for (const char* sub : kScanSubdirs) {
        auto sub_dir = data_dir / sub;
        std::error_code ec2;
        if (!std::filesystem::is_directory(sub_dir, ec2)) continue;
        for (auto it = std::filesystem::recursive_directory_iterator(sub_dir, ec2);
             it != std::filesystem::recursive_directory_iterator(); ++it) {
            if (ec2) break;
            if (!it->is_regular_file()) continue;
            auto rel = std::filesystem::relative(it->path(), data_dir, ec2);
            if (ec2) continue;
            std::string rels = rel.string();
            for (char& c : rels) if (c == '\\') c = '/';
            std::string low = to_lower(rels);
            if (manifest_paths.count(low) == 0) {
                std::string ext = it->path().extension().string();
                if (ext == ".gltf" || ext == ".json" || ext == ".png" || ext == ".bin") {
                    // .bin is the companion of .gltf — skip if a .gltf
                    // with the same stem is in the manifest.
                    if (ext == ".bin") {
                        std::string stem = low.substr(0, low.size() - 4);
                        if (listed_gltf_stems.count(stem)) continue;
                    }
                    add_finding(out, "D8", "",
                        "unlisted file on disk (no manifest entry): " + rels);
                }
            }
        }
    }
}

void check_d9_manifest_consistency(const std::filesystem::path& data_dir,
                                    const f4::assets::Manifest& manifest,
                                    DoctorReport& out) {
    if (manifest.format_version != f4::assets::kManifestFormatVersion) {
        add_finding(out, "D9", "",
            "manifest format_version is " + std::to_string(manifest.format_version) +
            ", expected " + std::to_string(f4::assets::kManifestFormatVersion));
    }
    if (manifest.data_dir.empty()) {
        add_finding(out, "D9", "", "manifest data_dir is empty");
    }
    auto mp = data_dir / "manifest.json";
    std::error_code ec;
    if (!std::filesystem::exists(mp, ec)) {
        add_finding(out, "D9", "", "manifest.json missing on disk");
    }
    for (const auto& a : manifest.assets) {
        if (!a.id.valid()) {
            add_finding(out, "D9", "",
                "manifest contains an asset entry with invalid id");
        }
    }
}

DoctorReport run_doctor(const std::filesystem::path& data_dir,
                        const f4::assets::Manifest& manifest) {
    DoctorReport out;
    check_d1_visual_bindings(data_dir, manifest, out);
    check_d2_world_json_refs(data_dir, manifest, out);
    check_d3_class_table_bindings(data_dir, manifest, out);
    check_d4_capability_sources(data_dir, manifest, out);
    check_d5_node_tags(data_dir, manifest, out);
    check_d6_vocab(data_dir, manifest, out);
    check_d7_orphans(data_dir, manifest, out);
    check_d8_id_and_files(data_dir, manifest, out);
    check_d9_manifest_consistency(data_dir, manifest, out);
    return out;
}

DoctorReport run_doctor(const std::filesystem::path& data_dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(data_dir, ec)) {
        DoctorReport out;
        add_finding(out, "D9", "",
            "Data/ directory does not exist: " + data_dir.string());
        return out;
    }
    auto manifest_path = data_dir / "manifest.json";
    if (!std::filesystem::exists(manifest_path, ec)) {
        DoctorReport out;
        add_finding(out, "D9", "",
            "manifest.json does not exist at " + manifest_path.string());
        return out;
    }
    f4::assets::Manifest m;
    try {
        m = f4::assets::read_manifest_file(manifest_path.string());
    } catch (const std::exception& e) {
        DoctorReport out;
        add_finding(out, "D9", "",
            std::string("manifest.json parse failed: ") + e.what());
        return out;
    }
    return run_doctor(data_dir, m);
}

std::string format_report(const DoctorReport& r) {
    std::ostringstream o;
    for (const auto& f : r.findings) {
        const char* sev = "?";
        switch (f.severity) {
            case Severity::error:   sev = "ERROR";   break;
            case Severity::warning: sev = "WARNING"; break;
            case Severity::info:    sev = "INFO";    break;
        }
        o << sev << " " << f.check_id;
        if (!f.asset_id.empty()) o << " [" << f.asset_id << "]";
        o << ": " << f.message << "\n";
    }
    o << "\nDoctor summary: " << r.errors << " errors, " << r.warnings
      << " warnings, " << r.infos << " infos ("
      << r.findings.size() << " findings)\n";
    return o.str();
}

} // namespace f4::import
