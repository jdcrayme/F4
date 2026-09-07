// f4-world-viewer/src/pipeline_io.cpp
//
// Implementation of pipeline_io.hpp — see the header for the
// "Data first, else convert into Data/Temp" contract.
//
// The converter CLIs (ct2json / terrain2json / cam2json / f4import) are
// pinned at configure time via generator expressions
// (F4_CT2JSON_EXE / F4_TERRAIN2JSON_EXE / F4_CAM2JSON_EXE /
// F4_F4IMPORT_EXE in f4-world-viewer/CMakeLists.txt) — the same
// binary-from-this-build-tree pattern the cam2json/terrain2json
// wiring used before Temp support was added.

#include <f4/viewer/pipeline_io.hpp>

#include <f4/assets/asset_id.hpp>
#include <f4/assets/asset_root.hpp>
#include <f4/install/installation.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace f4::viewer {

namespace {

// First + last slice of a captured converter log for error messages —
// `f4import --all` emits thousands of progress lines; the modal only
// needs the beginning (banner/arguments) and the end (the error).
std::string tail_for_error(const std::string& output) {
    constexpr std::size_t kKeep = 2000;
    if (output.size() <= 2 * kKeep) return output;
    return output.substr(0, kKeep) +
        "\n... [" + std::to_string(output.size() - 2 * kKeep) +
        " bytes elided] ...\n" +
        output.substr(output.size() - kKeep);
}

[[noreturn]] void throw_with_output(const std::string& what, int rc,
                                    const std::string& output) {
    std::string msg = what + " failed (exit " + std::to_string(rc) + ")";
    if (!output.empty()) msg += ":\n" + tail_for_error(output);
    throw std::runtime_error(msg);
}

std::filesystem::path require_data_dir() {
    auto data = discover_data_dir();
    if (data.empty() || !std::filesystem::exists(data)) {
        throw std::runtime_error(
            "Data/ directory not found (no AssetRoot from the working "
            "directory and no source-tree Data/) — nothing to convert "
            "into and nothing to read from. Run from the repo, or export "
            "the pipeline data with scripts/export-game-data.");
    }
    return data;
}

}  // namespace

// ── Paths ────────────────────────────────────────────────────────────────

std::filesystem::path discover_data_dir() {
    std::filesystem::path data_dir;
    if (auto root = f4::assets::AssetRoot::discover()) {
        data_dir = root->data_dir();
    }
    if (data_dir.empty() || !std::filesystem::exists(data_dir)) {
#ifdef F4_SOURCE_DIR
        data_dir = std::filesystem::path(F4_SOURCE_DIR) / "Data";
#endif
    }
    return data_dir;
}

std::filesystem::path temp_dir(const std::filesystem::path& data_dir) {
    return data_dir / "Temp";
}

std::string quote_arg(const std::filesystem::path& p) {
    const auto s = p.string();
    if (s.find(' ') == std::string::npos && s.find('"') == std::string::npos &&
        s.find('\t') == std::string::npos) {
        return s;
    }
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

// ── Process runner ───────────────────────────────────────────────────────

int run_converter(const std::filesystem::path& exe,
                  const std::string& args,
                  std::string* captured_output,
                  const ProgressFn& on_line) {
    std::string capture;      // everything the child printed
    std::size_t scanned = 0;  // bytes of `capture` already sent as lines
    auto pump = [&](const char* data, std::size_t n) {
        capture.append(data, n);
        if (on_line) {
            std::size_t start = scanned;
            for (;;) {
                const auto nl = capture.find('\n', start);
                if (nl == std::string::npos) break;
                on_line(capture.substr(start, nl - start));
                start = nl + 1;
            }
            scanned = start;
        }
    };

    int rc = -1;
#ifdef _WIN32
    {
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE read_end = nullptr;
        HANDLE write_end = nullptr;
        if (!CreatePipe(&read_end, &write_end, &inherit, 0)) {
            if (captured_output) *captured_output = "CreatePipe failed";
            return -1;
        }
        SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = write_end;
        si.hStdError = write_end;  // merged: one stream, interleaved output
        PROCESS_INFORMATION pi{};
        std::string cmd = quote_arg(exe) + " " + args;  // CreateProcessA may write in place
        const BOOL spawned = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr,
                                            TRUE, CREATE_NO_WINDOW, nullptr,
                                            nullptr, &si, &pi);
        CloseHandle(write_end);  // parent must drop its copy or ReadFile never EOFs
        if (!spawned) {
            CloseHandle(read_end);
            if (captured_output) {
                *captured_output = "failed to launch " + exe.string() +
                                   " (GetLastError=" + std::to_string(GetLastError()) + ")";
            }
            return -1;
        }

        char buf[4096];
        DWORD n = 0;
        while (ReadFile(read_end, buf, sizeof(buf), &n, nullptr) && n > 0) {
            pump(buf, n);
        }
        if (on_line && scanned < capture.size()) on_line(capture.substr(scanned));
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        rc = static_cast<int>(code);
        CloseHandle(read_end);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
#else
    {
        // popen goes through /bin/sh; quote_arg's double quotes are
        // shell-compatible, and 2>&1 merges stderr into the capture.
        const std::string cmd = quote_arg(exe) + " " + args + " 2>&1";
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) {
            if (captured_output) *captured_output = "popen failed for " + exe.string();
            return -1;
        }
        char buf[4096];
        while (std::fgets(buf, sizeof(buf), p)) {
            pump(buf, std::strlen(buf));
        }
        if (on_line && scanned < capture.size()) on_line(capture.substr(scanned));
        const int status = pclose(p);
        rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
#endif

    if (captured_output) *captured_output = std::move(capture);
    return rc;
}

// ── Resolvers ────────────────────────────────────────────────────────────

std::filesystem::path ensure_class_table_json(
    const f4::install::Installation& install,
    bool* converted_to_temp) {
    if (converted_to_temp) *converted_to_temp = false;
    const auto data = require_data_dir();

    const auto canonical = data / "Classes" / "falcon4.ct.json";
    std::error_code ec;
    if (std::filesystem::exists(canonical, ec)) return canonical;

    const auto out = temp_dir(data) / "Classes" / "falcon4.ct.json";
    if (!std::filesystem::exists(out, ec)) {
        const auto ct = install.class_table();
        if (ct.empty() || !std::filesystem::exists(ct)) {
            throw std::runtime_error(
                "No class table available: Data/Classes/falcon4.ct.json is "
                "missing and the install has no FALCON4.ct. Export the "
                "pipeline data with scripts/export-game-data, or check the "
                "install (Tools > Install Diagnostics).");
        }
        std::filesystem::create_directories(out.parent_path(), ec);
        std::string output;
        const int rc = run_converter(F4_CT2JSON_EXE,
                                     quote_arg(ct) + " " + quote_arg(out),
                                     &output);
        if (rc != 0) throw_with_output("ct2json", rc, output);
        if (converted_to_temp) *converted_to_temp = true;
    }
    return out;
}

std::filesystem::path ensure_terrain_json(
    const std::string& theater_key,
    const std::filesystem::path& theater_dir,
    bool* converted_to_temp) {
    if (converted_to_temp) *converted_to_temp = false;
    const auto data = require_data_dir();

    const auto canonical = data / "Theater" / theater_key / "terrain.json";
    std::error_code ec;
    if (std::filesystem::exists(canonical, ec)) return canonical;

    const auto out = temp_dir(data) / "Theater" / theater_key / "terrain.json";
    if (!std::filesystem::exists(out, ec)) {
        std::filesystem::create_directories(out.parent_path(), ec);
        std::string output;
        const int rc = run_converter(F4_TERRAIN2JSON_EXE,
                                     quote_arg(theater_dir) + " " + quote_arg(out),
                                     &output);
        if (rc != 0) throw_with_output("terrain2json", rc, output);
        if (converted_to_temp) *converted_to_temp = true;
    }
    return out;
}

std::filesystem::path ensure_world_json(
    const std::filesystem::path& cam_path,
    const std::filesystem::path& objects_dir,
    const std::filesystem::path& binary_ct,
    bool* converted_to_temp) {
    if (converted_to_temp) *converted_to_temp = false;
    const auto data = require_data_dir();
    const std::string stem = cam_path.stem().string();
    std::error_code ec;

    // Canonical exports first: the manifest is the authority (P7) — it
    // alone knows WHICH .cam an export came from, so a hit here proves
    // the export belongs to this campaign (campaign ids are the
    // LOWERCASED stem, per f4::import::campaign_id_from_cam_path).
    // Stems that can never be a local_id ("Auto Save" — uppercase/
    // space, grammar is [a-z0-9._-]) simply have no manifest entry:
    // skip the lookup, don't throw — parse_asset_id rejects such ids
    // and the conversion fallback below handles them fine. Deliberately
    // NO "<theater>.world.json" filename fallback: every campaign in a
    // theater would match the same canonical export and silently load
    // the wrong save.
    std::string lower_stem = stem;
    std::transform(lower_stem.begin(), lower_stem.end(), lower_stem.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    // NOTE: AssetId::valid() does NOT enforce the local_id grammar —
    // gate on is_valid_local_id explicitly.
    if (f4::assets::is_valid_local_id(lower_stem)) {
        const f4::assets::AssetId campaign_id(
            f4::assets::AssetFamily::campaign, lower_stem);
        if (auto root = f4::assets::AssetRoot::at(data)) {
            auto p = root->resolve_existing(campaign_id);
            if (!p.empty()) return p;
        }
    }
    // Trees whose manifest predates campaign entries may still carry a
    // per-stem export next to the well-known World/ dir — accept it
    // only under this campaign's own stem.
    const auto stem_export = data / "World" / (stem + ".world.json");
    if (std::filesystem::exists(stem_export, ec)) return stem_export;

    const auto out = temp_dir(data) / "World" / (stem + ".world.json");
    if (!std::filesystem::exists(out, ec)) {
        std::filesystem::create_directories(out.parent_path(), ec);
        std::string args = quote_arg(cam_path) + " " + quote_arg(out);
        if (!objects_dir.empty()) {
            args += " --theater-data " + quote_arg(objects_dir);
        }
        if (!binary_ct.empty()) {
            args += " --class-table " + quote_arg(binary_ct);
        }
        std::string output;
        const int rc = run_converter(F4_CAM2JSON_EXE, args, &output);
        if (rc != 0) throw_with_output("cam2json", rc, output);
        if (converted_to_temp) *converted_to_temp = true;
    }
    return out;
}

std::filesystem::path find_models_data_root(const std::filesystem::path& data_dir) {
    std::error_code ec;
    if (std::filesystem::exists(data_dir / "Models" / "koreaobj", ec)) {
        return data_dir;
    }
    const auto tmp = temp_dir(data_dir);
    if (std::filesystem::exists(tmp / "Models" / "koreaobj", ec)) {
        return tmp;
    }
    return {};
}

std::filesystem::path ensure_models_root(const f4::install::Installation& install,
                                         const ProgressFn& on_line) {
    const auto data = require_data_dir();
    std::error_code ec;

    // Canonical export (or a previous completed Temp conversion) — done.
    if (auto existing = find_models_data_root(data); !existing.empty()) {
        return existing;
    }

    // Convert into a staging dir first and move into place on success —
    // `f4import --all` writes thousands of files over minutes, and a
    // half-written Temp/Models must never look like a finished one.
    const auto staging = temp_dir(data) / ".models-staging";
    const auto final_models = temp_dir(data) / "Models";
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(staging, ec);

    auto convert = [&](const char* exe, const char* subcmd, const char* what) {
        const std::string args = std::string(subcmd) + " --install " +
                                 quote_arg(install.root()) + " --data " +
                                 quote_arg(staging) + " --all";
        std::string output;
        const int rc = run_converter(exe, args, &output, on_line);
        if (rc != 0) throw_with_output(what, rc, output);
    };
    convert(F4_F4IMPORT_EXE, "textures", "f4import textures");
    convert(F4_F4IMPORT_EXE, "models", "f4import models");

    std::filesystem::remove_all(final_models, ec);
    std::filesystem::rename(staging / "Models", final_models, ec);
    if (!std::filesystem::exists(final_models / "koreaobj", ec)) {
        throw std::runtime_error(
            "f4import finished but Data/Temp/Models/koreaobj is missing — "
            "the install may have no KoreaObj model data.");
    }
    std::filesystem::remove_all(staging, ec);
    return temp_dir(data);
}

} // namespace f4::viewer
