// f4-import/cli/f4import.cpp
//
// f4import — the importer CLI.
//
// Stage 0 surface:
//   f4import check   --data <dir> [--json]
//   f4import doctor  --data <dir> [--json]
//
// Stage 3 surface:
//   f4import models   --install <root> --data <dir> [--model <N>] [--all]
//     Convert KoreaObj models to glTF and write to <Data>/Models/koreaobj/.
//   f4import textures --install <root> --data <dir> [--texture <N>] [--all]
//     Decode KoreaObj.TEX entries to PNG and write to
//     <Data>/Models/koreaobj/textures/ (referenced by the glTF materials).
//
// Stage 1+ adds import / ensure with the full converter pipeline.

#include <f4/assets/asset_root.hpp>
#include <f4/assets/manifest.hpp>
#include <f4/import/doctor.hpp>
#include <f4/import/gltf_emitter.hpp>
#include <f4/import/manifest_writer.hpp>
#include <f4/import/texture_png.hpp>
#include <f4/models/model_database.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void print_usage() {
    std::cerr <<
        "f4import — F4 asset pipeline CLI\n"
        "\n"
        "Usage:\n"
        "  f4import check   --data <dir> [--json]\n"
        "    Exit 0 if manifest ok, 1 if missing/stale, 2 warnings only.\n"
        "\n"
        "  f4import doctor  --data <dir> [--json]\n"
        "    Run D1-D9 validation against a Data/ tree.\n"
        "    Exit 0 clean, 1 errors, 2 warnings only.\n"
        "\n"
        "  f4import models  --install <root> --data <dir> [--model <N>] [--all]\n"
        "    Convert KoreaObj models to glTF. Writes to <Data>/Models/koreaobj/.\n"
        "    --model <N>  Convert a single model by index.\n"
        "    --all        Convert all models in the database.\n"
        "    (Without --model or --all, converts model 0 as a smoke test.)\n"
        "\n"
        "  f4import textures --install <root> --data <dir> [--texture <N>] [--all]\n"
        "    Decode KoreaObj.TEX textures to PNG. Writes to\n"
        "    <Data>/Models/koreaobj/textures/ (referenced by the glTF materials).\n"
        "    --texture <N>  Export a single texture by bank index.\n"
        "    --all          Export all textures in the bank.\n"
        "    (Without --texture or --all, exports texture 0 as a smoke test.)\n"
        "\n"
        "  --json  Emit machine-readable JSON instead of text.\n";
}

std::filesystem::path parse_data_dir(int argc, char** argv, int start) {
    for (int i = start; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--data" && i + 1 < argc) {
            return std::filesystem::path(argv[++i]);
        }
        if (a.substr(0, 7) == "--data=") {
            return std::filesystem::path(a.substr(7));
        }
    }
    return {};
}

bool want_json(int argc, char** argv, int start) {
    for (int i = start; i < argc; ++i) {
        if (std::string(argv[i]) == "--json") return true;
    }
    return false;
}

int run_check(const std::filesystem::path& data_dir, bool json) {
    auto root = f4::assets::AssetRoot::at(data_dir);
    if (!root) {
        if (json) {
            std::cout << "{\"status\":\"missing\",\"detail\":\"Data/ directory not found\"}\n";
        } else {
            std::cerr << "f4import check: Data/ directory not found: "
                      << data_dir << "\n";
        }
        return 1;
    }
    if (root->manifest().assets.empty()) {
        if (json) {
            std::cout << "{\"status\":\"empty\",\"detail\":\"manifest has no assets\"}\n";
        } else {
            std::cout << "Data/ found, manifest is empty (run f4import import to populate)\n";
        }
        return 1;
    }
    if (json) {
        std::cout << "{\"status\":\"ok\",\"assets\":"
                  << root->manifest().assets.size() << "}\n";
    } else {
        std::cout << "OK — manifest loaded, "
                  << root->manifest().assets.size() << " assets in "
                  << data_dir << "\n";
    }
    return 0;
}

int run_doctor(const std::filesystem::path& data_dir, bool json) {
    auto report = f4::import::run_doctor(data_dir);
    if (json) {
        std::cout << "{\"errors\":" << report.errors
                  << ",\"warnings\":" << report.warnings
                  << ",\"infos\":" << report.infos
                  << ",\"findings\":[";
        for (std::size_t i = 0; i < report.findings.size(); ++i) {
            const auto& f = report.findings[i];
            if (i) std::cout << ",";
            const char* sev = "?";
            switch (f.severity) {
                case f4::import::Severity::error:   sev = "error";   break;
                case f4::import::Severity::warning: sev = "warning"; break;
                case f4::import::Severity::info:    sev = "info";    break;
            }
            std::cout << "{\"check\":\"" << f.check_id << "\","
                      << "\"severity\":\"" << sev << "\","
                      << "\"asset\":\"" << f.asset_id << "\","
                      << "\"message\":\"" << f.message << "\"}";
        }
        std::cout << "]}\n";
    } else {
        std::cout << f4::import::format_report(report);
    }
    if (report.has_errors()) return 1;
    if (report.has_warnings_only()) return 2;
    return 0;
}

} // namespace

// ── models subcommand ─────────────────────────────────────────────────────

namespace {

std::filesystem::path parse_install(int argc, char** argv, int start) {
    for (int i = start; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--install" && i + 1 < argc) {
            return std::filesystem::path(argv[++i]);
        }
        if (a.substr(0, 9) == "--install=") {
            return std::filesystem::path(a.substr(9));
        }
    }
    return {};
}

int parse_model_index(int argc, char** argv, int start) {
    for (int i = start; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--model" && i + 1 < argc) {
            return std::atoi(argv[++i]);
        }
        if (a.substr(0, 8) == "--model=") {
            return std::atoi(a.substr(8).c_str());
        }
    }
    return -1;
}

int parse_texture_index(int argc, char** argv, int start) {
    for (int i = start; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--texture" && i + 1 < argc) {
            return std::atoi(argv[++i]);
        }
        if (a.substr(0, 10) == "--texture=") {
            return std::atoi(a.substr(10).c_str());
        }
    }
    return -1;
}

bool want_all(int argc, char** argv, int start) {
    for (int i = start; i < argc; ++i) {
        if (std::string(argv[i]) == "--all") return true;
    }
    return false;
}

int run_models(int argc, char** argv) {
    namespace fs = std::filesystem;
    auto install_root = parse_install(argc, argv, 2);
    auto data_dir = parse_data_dir(argc, argv, 2);
    if (install_root.empty() || data_dir.empty()) {
        std::cerr << "f4import models: --install <root> and --data <dir> are required\n";
        return 3;
    }

    int model_idx = parse_model_index(argc, argv, 2);
    bool all = want_all(argc, argv, 2);

    try {
        // Find KoreaObj.HDR/LOD in the install.
        auto [hdr_path, lod_path] = f4::models::ModelDatabase::find_koreaobj_files(install_root);
        if (hdr_path.empty() || lod_path.empty()) {
            std::cerr << "f4import models: KoreaObj.HDR/LOD not found in "
                      << install_root << "\n";
            return 1;
        }

        // Load + parse the model database.
        f4::models::ModelDatabase db;
        std::string err = db.load(hdr_path, lod_path);
        if (!err.empty()) {
            std::cerr << "f4import models: load failed: " << err << "\n";
            return 1;
        }
        std::cerr << "  loaded " << db.n_models() << " models from "
                  << hdr_path << "\n";

        // Determine which models to convert.
        std::vector<int> indices;
        if (all) {
            for (int i = 0; i < db.n_models(); ++i) indices.push_back(i);
        } else if (model_idx >= 0) {
            indices.push_back(model_idx);
        } else {
            indices.push_back(0);  // smoke test: model 0
        }

        // Output directory: <Data>/Models/koreaobj/
        fs::path out_dir = data_dir / "Models" / "koreaobj";
        fs::create_directories(out_dir);

        // Convert each model.
        std::size_t ok = 0, fail = 0;
        for (int idx : indices) {
            // Parse the model first (must be done before extraction).
            err = db.parse_model(idx);
            if (!err.empty()) {
                std::cerr << "  model " << idx << ": parse failed: " << err << "\n";
                ++fail;
                continue;
            }

            // Build the asset ID: koreaobj:NNNNN (zero-padded 5).
            char id_buf[32];
            std::snprintf(id_buf, sizeof(id_buf), "koreaobj:%05d", idx);
            std::string asset_id = id_buf;

            try {
                f4::import::GltfEmitOptions opts;
                auto result = f4::import::emit_model_as_gltf(
                    db, idx, out_dir, asset_id, opts);

                // Update the manifest.
                std::vector<f4::assets::Capability> caps;
                caps.push_back({"dofs", f4::assets::CapabilityStatus::present,
                                static_cast<int>(result.dof_count)});
                caps.push_back({"switches", f4::assets::CapabilityStatus::present,
                                static_cast<int>(result.switch_count)});
                caps.push_back({"slots", f4::assets::CapabilityStatus::present,
                                static_cast<int>(result.slot_count)});
                caps.push_back({"anchors", f4::assets::CapabilityStatus::unknown});

                std::vector<f4::assets::AssetSource> sources;
                sources.push_back({hdr_path.string(), "art", ""});
                sources.push_back({lod_path.string(), "art", ""});

                std::string rel_path = "Models/koreaobj/" +
                    std::string(id_buf + 9) + ".gltf";  // "00002.gltf"
                (void)f4::import::update_manifest_for_asset(
                    data_dir,
                    f4::assets::AssetId{f4::assets::AssetFamily::koreaobj,
                                         std::string(id_buf + 9)},
                    rel_path,
                    /*format_version=*/1,
                    std::move(caps),
                    std::move(sources),
                    /*generator=*/"f4import models 0.5.0");

                std::cout << "  model " << idx << ": " << result.total_vertices
                          << " verts, " << result.total_triangles << " tris, "
                          << result.lod_count << " LODs, "
                          << result.dof_count << " DOFs, "
                          << result.switch_count << " switches, "
                          << result.slot_count << " slots -> "
                          << result.gltf_path << "\n";
                ++ok;
            } catch (const std::exception& e) {
                std::cerr << "  model " << idx << ": emit failed: " << e.what() << "\n";
                ++fail;
            }
        }

        std::cerr << "  manifest:    updated (" << (data_dir / "manifest.json") << ")\n";
        std::cout << "Converted " << ok << " models (" << fail << " failures)\n";
        return fail > 0 ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << "f4import models: error: " << e.what() << "\n";
        return 1;
    }
}

} // namespace

// ── textures subcommand ───────────────────────────────────────────────────

namespace {

int run_textures(int argc, char** argv) {
    namespace fs = std::filesystem;
    auto install_root = parse_install(argc, argv, 2);
    auto data_dir = parse_data_dir(argc, argv, 2);
    if (install_root.empty() || data_dir.empty()) {
        std::cerr << "f4import textures: --install <root> and --data <dir> are required\n";
        return 3;
    }

    int tex_idx = parse_texture_index(argc, argv, 2);
    bool all = want_all(argc, argv, 2);

    try {
        // KoreaObj.HDR provides the texture bank + palettes; KoreaObj.TEX
        // provides the LZSS-compressed pixel blobs.
        auto [hdr_path, lod_path] = f4::models::ModelDatabase::find_koreaobj_files(install_root);
        if (hdr_path.empty()) {
            std::cerr << "f4import textures: KoreaObj.HDR not found in "
                      << install_root << "\n";
            return 1;
        }
        auto tex_path = f4::models::ModelDatabase::find_tex_file(install_root);
        if (tex_path.empty()) {
            std::cerr << "f4import textures: KoreaObj.TEX not found in "
                      << install_root << "\n";
            return 1;
        }

        f4::models::ModelDatabase db;
        std::string err = db.load_hdr(hdr_path);
        if (!err.empty()) {
            std::cerr << "f4import textures: HDR load failed: " << err << "\n";
            return 1;
        }
        err = db.load_tex(tex_path);
        if (!err.empty()) {
            std::cerr << "f4import textures: TEX load failed: " << err << "\n";
            return 1;
        }
        std::cerr << "  loaded " << db.n_textures() << " textures from "
                  << tex_path << "\n";

        // Determine which textures to export.
        std::vector<int> indices;
        if (all) {
            for (int i = 0; i < db.n_textures(); ++i) indices.push_back(i);
        } else if (tex_idx >= 0) {
            if (tex_idx >= db.n_textures()) {
                std::cerr << "f4import textures: texture " << tex_idx
                          << " out of range (bank has " << db.n_textures() << ")\n";
                return 1;
            }
            indices.push_back(tex_idx);
        } else {
            indices.push_back(0);  // smoke test: texture 0
        }

        // Output directory: <Data>/Models/koreaobj/textures/ — the glTF
        // materials reference these as "textures/NNNNN.png".
        fs::path out_dir = data_dir / "Models" / "koreaobj" / "textures";

        std::size_t ok = 0, fail = 0;
        for (int idx : indices) {
            // Asset id: NNNNN.png — distinct from model local ids ("NNNNN")
            // within the same koreaobj family namespace.
            char id_buf[32];
            std::snprintf(id_buf, sizeof(id_buf), "%05d.png", idx);

            try {
                auto result = f4::import::write_texture_png(db, idx, out_dir);

                std::vector<f4::assets::Capability> caps;
                caps.push_back({"alpha", result.has_alpha
                                             ? f4::assets::CapabilityStatus::present
                                             : f4::assets::CapabilityStatus::none});
                caps.push_back({"chroma_key", result.chroma_keyed
                                                  ? f4::assets::CapabilityStatus::present
                                                  : f4::assets::CapabilityStatus::none});

                std::vector<f4::assets::AssetSource> sources;
                sources.push_back({tex_path.string(), "art", ""});
                sources.push_back({hdr_path.string(), "art", ""});

                std::string rel_path = "Models/koreaobj/textures/" + std::string(id_buf);
                (void)f4::import::update_manifest_for_asset(
                    data_dir,
                    f4::assets::AssetId{f4::assets::AssetFamily::koreaobj,
                                         std::string(id_buf)},
                    rel_path,
                    /*format_version=*/1,
                    std::move(caps),
                    std::move(sources),
                    /*generator=*/"f4import textures 0.5.0");

                std::cout << "  texture " << idx << ": " << result.width << "x"
                          << result.height
                          << (result.has_alpha ? " (alpha)" : "")
                          << (result.chroma_keyed ? " (chroma-keyed)" : "")
                          << " -> " << result.png_path << "\n";
                ++ok;
            } catch (const std::exception& e) {
                std::cerr << "  texture " << idx << ": failed: " << e.what() << "\n";
                ++fail;
            }
        }

        std::cout << "Exported " << ok << " textures (" << fail << " failures)\n";
        return fail > 0 ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << "f4import textures: error: " << e.what() << "\n";
        return 1;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 3;
    }
    const std::string sub = argv[1];
    if (sub == "--help" || sub == "-h" || sub == "help") {
        print_usage();
        return 0;
    }
    if (sub == "check") {
        auto data_dir = parse_data_dir(argc, argv, 2);
        if (data_dir.empty()) {
            std::cerr << "f4import check: --data <dir> is required\n";
            return 3;
        }
        return run_check(data_dir, want_json(argc, argv, 2));
    }
    if (sub == "doctor") {
        auto data_dir = parse_data_dir(argc, argv, 2);
        if (data_dir.empty()) {
            std::cerr << "f4import doctor: --data <dir> is required\n";
            return 3;
        }
        return run_doctor(data_dir, want_json(argc, argv, 2));
    }
    if (sub == "models") {
        return run_models(argc, argv);
    }
    if (sub == "textures") {
        return run_textures(argc, argv);
    }
    std::cerr << "f4import: unknown subcommand '" << sub << "'\n";
    print_usage();
    return 3;
}
