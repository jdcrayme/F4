// f4-install/src/installation.cpp

#include <f4/install/installation.hpp>
#include <f4/install/file_finder.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

namespace f4::install {

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// DiagnosticInfo::format
// ---------------------------------------------------------------------------
std::string DiagnosticInfo::format() const {
    std::ostringstream ss;
    ss << "FALCON4.ct search (" << class_table_searched.size() << " locations probed):\n";
    if (class_table_searched.empty()) {
        ss << "  (no locations probed — install not detected)\n";
    } else {
        for (const auto& p : class_table_searched) {
            ss << "  " << p.string() << "\n";
        }
    }
    ss << "\n";
    ss << "theater.lst: ";
    if (theater_lst_path.empty()) {
        ss << "not found (fell back to directory scan)\n";
    } else {
        ss << theater_lst_path.string();
        if (theater_lst_parsed) {
            ss << " (parsed, " << theater_lst_key_count << " keys)\n";
        } else {
            ss << " (found but not parsed)\n";
        }
    }
    ss << "\n";
    ss << "Theater dirs probed: " << theater_dirs_probed.size() << "\n";
    ss << "Campaign dir found: " << (campaign_dir_found ? "yes" : "no") << "\n";
    return ss.str();
}

// ---------------------------------------------------------------------------
// Installation::detect
// ---------------------------------------------------------------------------
Installation Installation::detect(const std::filesystem::path& root) {
    Installation inst;
    inst.root_ = root;

    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return inst;  // valid() == false
    }

    // 1. FALCON4.ct — the class table. Try root, then sim/, then terrdata/.
    //    Different install layouts put it in different places; the canonical
    //    vanilla F4 location is the install root.
    {
        const char* filename = "FALCON4.ct";
        inst.diagnostics_.class_table_searched.push_back(root / filename);
        inst.class_table_ = find_file_ci(root, filename);
        if (inst.class_table_.empty()) {
            auto sim = find_subdir_ci(root, "sim");
            if (!sim.empty()) {
                inst.aircraft_dir_ = sim;
                inst.diagnostics_.class_table_searched.push_back(sim / filename);
                inst.class_table_ = find_file_ci(sim, filename);
            }
        }
        if (inst.class_table_.empty()) {
            auto td = find_subdir_ci(root, "terrdata");
            if (!td.empty()) {
                inst.diagnostics_.class_table_searched.push_back(td / filename);
                inst.class_table_ = find_file_ci(td, filename);

                if (inst.class_table_.empty()) {
                    td = find_subdir_ci(td, "objects");
                    if (!td.empty()) {
                        inst.diagnostics_.class_table_searched.push_back(td / filename);
                        inst.class_table_ = find_file_ci(td, filename);
                    }
                }
            }
        }
    }

    // 2. sim/ — aircraft data (.dat files). We always locate this even
    //    if we already did while probing for FALCON4.ct above.
    if (inst.aircraft_dir_.empty()) {
        inst.aircraft_dir_ = find_subdir_ci(root, "sim");
    }

    // 3. terrdata/ — theater data. Required for world visualization.
    inst.terrdata_dir_ = find_subdir_ci(root, "terrdata");
    if (!inst.terrdata_dir_.empty()) {
        // Parse theater.lst for preferred ordering + display names.
        auto lst = find_file_ci(inst.terrdata_dir_, "theater.lst");
        std::vector<std::string> preferred;
        if (!lst.empty()) {
            inst.diagnostics_.theater_lst_path = lst;
            preferred = parse_theater_lst(lst);
            inst.diagnostics_.theater_lst_parsed = true;
            inst.diagnostics_.theater_lst_key_count = preferred.size();
        }
        inst.theaters_ = scan_theaters(inst.terrdata_dir_, preferred);
        // Record every subdir we looked at (for transparency, even the
        // ones we rejected because they had no THEATER.MAP).
        std::error_code ec2;
        for (const auto& entry : std::filesystem::directory_iterator(inst.terrdata_dir_, ec2)) {
            if (entry.is_directory()) {
                inst.diagnostics_.theater_dirs_probed.push_back(entry.path());
            }
        }
    }

    // 4. campaign/ — saved campaigns. May be flat (vanilla) or nested
    //    per-theater (FreeFalcon multi-theater).
    inst.campaign_dir_ = find_subdir_ci(root, "campaign");
    inst.diagnostics_.campaign_dir_found = !inst.campaign_dir_.empty();
    if (!inst.campaign_dir_.empty()) {
        std::vector<std::string> keys;
        keys.reserve(inst.theaters_.size());
        for (const auto& t : inst.theaters_) keys.push_back(t.key);
        inst.campaigns_ = scan_campaigns(inst.campaign_dir_, keys);
    }

    return inst;
}

bool Installation::valid() const noexcept {
    return !class_table_.empty() || !terrdata_dir_.empty();
}

const Theater* Installation::find_theater(const std::string& key) const noexcept {
    const std::string k = to_lower(key);
    for (const auto& t : theaters_) {
        if (t.key == k) return &t;
    }
    return nullptr;
}

std::vector<Campaign> Installation::campaigns_for(const std::string& theater_key) const {
    std::vector<Campaign> out;
    const std::string k = to_lower(theater_key);
    for (const auto& c : campaigns_) {
        if (c.theater_key == k || c.theater_key.empty()) {
            out.push_back(c);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// find_class_table — install-aware resolver
//
// Stage 2 (ASSET_PIPELINE_SPEC.md §12): the CWD fallback list that
// used to be step 4 here is DELETED. The install-aware resolver now
// searches:
//   1. Same directory as the reference file (typically the .cam)
//   2. Up a directory or two from the reference file
//   3. The install's class_table() path (resolved during detect())
// and returns an empty path if none of those find it. Callers that
// want the legacy CWD-relative search (cam2json run from the build dir
// against bundled fixtures) should call the free function
// find_class_table_cwd_fallback() explicitly — that function still
// exists, but the install no longer silently falls back to it.
// ---------------------------------------------------------------------------
std::filesystem::path Installation::find_class_table(
    const std::filesystem::path& reference_file) const {
    const std::string filename = "FALCON4.ct";

    // 1. Same directory as the reference file (typically the .cam).
    if (!reference_file.empty()) {
        auto dir = reference_file.parent_path();
        if (!dir.empty()) {
            auto p = find_file_ci(dir, filename);
            if (!p.empty()) return p;
        }
        // 2. Up a directory or two.
        auto parent = reference_file.parent_path().parent_path();
        if (!parent.empty()) {
            auto p = find_file_ci(parent, filename);
            if (!p.empty()) return p;
        }
        auto grandparent = reference_file.parent_path().parent_path().parent_path();
        if (!grandparent.empty()) {
            auto p = find_file_ci(grandparent, filename);
            if (!p.empty()) return p;
        }
    }

    // 3. The install's class_table() path (resolved during detect()).
    if (!class_table_.empty()) {
        return class_table_;
    }

    // (Stage 2: the CWD fallback is no longer called here. Callers that
    // need it should call f4::install::find_class_table_cwd_fallback()
    // explicitly — see the free-function documentation.)
    return {};
}

std::filesystem::path Installation::resolve(const std::string& relative) const {
    if (!valid()) return {};
    return root_ / relative;
}

// ---------------------------------------------------------------------------
// Free-function helpers
// ---------------------------------------------------------------------------
std::filesystem::path find_class_table_in_install(
    const std::filesystem::path& root,
    const std::filesystem::path& reference_file) {
    auto inst = Installation::detect(root);
    return inst.find_class_table(reference_file);
}

std::filesystem::path find_class_table_cwd_fallback() {
    // Legacy CWD-relative search — preserved for the cam2json no-install
    // workflow (running against bundled test fixtures from the build
    // dir). Stage 2 removes this from Installation::find_class_table()'s
    // automatic fallback chain; callers who want this behavior must
    // call this free function explicitly. The asset-pipeline mode
    // (cam2json --data-dir) supersedes it — the manifest is the single
    // source of truth for "where is FALCON4.ct" in that flow.
    const char* candidates[] = {
        "FALCON4.ct",
        "assets/FALCON4.ct",
        "temp/FALCON4.ct",
        "f4-world-convert/tests/fixtures/FALCON4.ct",
        "../f4-world-convert/tests/fixtures/FALCON4.ct",
        "../../f4-world-convert/tests/fixtures/FALCON4.ct",
        "../temp/FALCON4.ct",
        "../../temp/FALCON4.ct",
    };
    for (const char* rel : candidates) {
        if (std::filesystem::exists(rel)) {
            return std::filesystem::path(rel);
        }
    }
    return {};
}

} // namespace f4::install
