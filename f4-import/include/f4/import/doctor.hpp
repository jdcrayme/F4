// f4-import/include/f4/import/doctor.hpp
//
// doctor — the manifest + Data/ tree validator. Runs the D1-D9 checks
// from ASSET_PIPELINE_SPEC §9.2 against a Data/ directory.

#pragma once

#include <f4/assets/manifest.hpp>

#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>

namespace f4::import {

enum class Severity {
    info,
    warning,
    error
};

struct DoctorFinding {
    std::string check_id;
    Severity severity;
    std::string asset_id;
    std::string message;
};

struct DoctorReport {
    std::vector<DoctorFinding> findings;
    std::size_t errors = 0;
    std::size_t warnings = 0;
    std::size_t infos = 0;

    [[nodiscard]] bool has_errors() const noexcept { return errors > 0; }
    [[nodiscard]] bool has_warnings_only() const noexcept {
        return warnings > 0 && errors == 0;
    }
};

[[nodiscard]] std::string format_report(const DoctorReport& r);

[[nodiscard]] DoctorReport run_doctor(const std::filesystem::path& data_dir);
[[nodiscard]] DoctorReport run_doctor(const std::filesystem::path& data_dir,
                                       const f4::assets::Manifest& manifest);

void check_d1_visual_bindings(const std::filesystem::path& data_dir,
                                const f4::assets::Manifest& manifest,
                                DoctorReport& out);
void check_d2_world_json_refs(const std::filesystem::path& data_dir,
                                const f4::assets::Manifest& manifest,
                                DoctorReport& out);
void check_d3_class_table_bindings(const std::filesystem::path& data_dir,
                                     const f4::assets::Manifest& manifest,
                                     DoctorReport& out);
void check_d4_capability_sources(const std::filesystem::path& data_dir,
                                   const f4::assets::Manifest& manifest,
                                   DoctorReport& out);
void check_d5_node_tags(const std::filesystem::path& data_dir,
                         const f4::assets::Manifest& manifest,
                         DoctorReport& out);
void check_d6_vocab(const std::filesystem::path& data_dir,
                     const f4::assets::Manifest& manifest,
                     DoctorReport& out);
void check_d7_orphans(const std::filesystem::path& data_dir,
                       const f4::assets::Manifest& manifest,
                       DoctorReport& out);
void check_d8_id_and_files(const std::filesystem::path& data_dir,
                            const f4::assets::Manifest& manifest,
                            DoctorReport& out);
void check_d9_manifest_consistency(const std::filesystem::path& data_dir,
                                    const f4::assets::Manifest& manifest,
                                    DoctorReport& out);

} // namespace f4::import
