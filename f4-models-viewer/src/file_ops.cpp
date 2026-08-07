// f4-models-viewer/src/file_ops.cpp
//
// File operations — loading model database from HDR/LOD files and
// finding KoreaObj files from an install path. After HDR/LOD are
// loaded, automatically attempts to load the TEX file for textures.

#include "viewer_state.hpp"
#include "file_ops.hpp"

#include <f4/models/model_database.hpp>

#include <utility>

namespace f4::models_viewer {

// ── find_koreaobj_files ────────────────────────────────────────────────────
std::pair<std::filesystem::path, std::filesystem::path>
find_koreaobj_files(const std::filesystem::path& install_root) {
    return f4::models::ModelDatabase::find_koreaobj_files(install_root);
}

// ── Impl::load_model_files ─────────────────────────────────────────────────
void ViewerApp::Impl::load_model_files(
    const std::filesystem::path& hdr_path,
    const std::filesystem::path& lod_path)
{
    // Unload previous
    unload_meshes();
    raylib_meshes.clear();
    doc_loaded = false;
    selected_parent = -1;
    selected_lod = 0;
    model_state = {};
    meshes_dirty = true;

    // Load the database
    std::string err = db.load(hdr_path, lod_path);
    if (!err.empty()) {
        status_msg = "Load error: " + err;
        return;
    }

    doc_loaded = true;
    status_msg = "Loaded " + std::to_string(db.n_models()) +
                 " models from " + hdr_path.filename().string();

    // Auto-load TEX file if found next to the HDR
    auto tex_path = db.find_tex_next_to_hdr();
    if (!tex_path.empty()) {
        std::string tex_err = db.load_tex(tex_path);
        if (tex_err.empty()) {
            status_msg += " + TEX (" +
                         std::to_string(db.tex_entries().size()) + " textures)";
        } else {
            status_msg += " | TEX load failed: " + tex_err;
        }
    }

    // Auto-select first model if available
    if (db.n_models() > 0) {
        selected_parent = 0;
        selected_lod = 0;
        meshes_dirty = true;

        // Initialize model state from the first model's DOFs/switches
        const auto* rec = db.model(0);
        if (rec) {
            model_state = {};
            for (int i = 0; i < rec->effective_dofs(); ++i) {
                f4::models::DofState ds;
                ds.dof_number = i;
                ds.value = 0;
                ds.min = 0;
                ds.max = 6.28318530718f;  // 2π
                model_state.dofs.push_back(ds);
            }
            for (int i = 0; i < rec->effective_switches(); ++i) {
                f4::models::SwitchState ss;
                ss.switch_number = i;
                ss.active_child = 0;
                ss.n_children = 2;  // default assumption
                model_state.switches.push_back(ss);
            }
        }

        // Fit camera to first model
        fit_to_model();
    }
}

// ── Impl::load_from_install ────────────────────────────────────────────────
void ViewerApp::Impl::load_from_install() {
    if (!install.has_value()) return;

    auto [hdr, lod] = find_koreaobj_files(install->root());
    if (hdr.empty() || lod.empty()) {
        status_msg = "KoreaObj.HDR/LOD not found in install";
        return;
    }

    load_model_files(hdr, lod);
}

} // namespace f4::models_viewer
