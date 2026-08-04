// f4-world-viewer/src/snapshot.cpp
//
// Implementation of build_install_snapshot() — see snapshot.hpp for the
// full rationale. This file is intentionally free of viewer deps (no
// raylib, no imgui, no ViewerApp::Impl) so it can be unit-tested in
// isolation if we ever want to, and so the --snapshot CLI flag can
// run headless without initializing a window.
//
// The function is "expensive" (reads ~15 files of up to 8 KB each =
// ~120 KB of I/O) but cheap relative to a single render frame, so we
// don't bother with progress callbacks — the caller either runs it
// synchronously (CLI) or in the menu-item callback (GUI, which stalls
// the render loop for a few hundred ms, tolerable).

#include "snapshot.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ios>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace f4::viewer {

namespace {

// -------------------------------------------------------------------------
// Curated target list — files we know we want bytes from for the
// upcoming static-data parsers. Each entry is a relative path under the
// install root. Case-insensitive matching is used when checking for
// existence (some installs mix case).
//
// Keep this list in sync with Docs/FALCON4_FILE_LAYOUT.md.
// -------------------------------------------------------------------------
constexpr std::array<const char*, 17> kCuratedFiles = {
    // Class table — already parsed by f4-world-convert, but useful to
    // include in the snapshot for cross-reference with objectives.
    "FALCON4.ct",

    // Static per-theater object data (under terrdata/objects/):
    "terrdata/objects/Falcon4.PHD",   // PtHeaderDataTable — airbase layout headers
    "terrdata/objects/Falcon4.PD",    // PtDataTable — runway/taxi/parking points
    "terrdata/objects/Falcon4.OCD",   // ObjClassDataType — objective class names + data
    "terrdata/objects/Falcon4.UCD",   // UnitClassDataType — unit class composition
    "terrdata/objects/Falcon4.VCD",   // VehicleClassDataType — per-vehicle data
    "terrdata/objects/Falcon4.FED",   // FeatureClassDataType — feature class data
    "terrdata/objects/Falcon4.FCD",   // FeatureDataType — feature definitions
    "terrdata/objects/Falcon4.OTD",   // ObjectiveTypeData — type table (may be absent)
    "terrdata/objects/Falcon4.ORD",   // Objective Radar data (may be absent)
    "terrdata/objects/Falcon4.RCD",   // RadarClassData (may be absent)

    // AI / simulation tuning (under terrdata/ai/):
    "terrdata/ai/Falcon4.AII",        // AI ini — SIM_BUBBLE_SIZE etc.
    "terrdata/ai/Falcon4.AIL",        // AI logic (may be absent)
    "terrdata/ai/Falcon4.SAI",        // (may be absent)

    // Theater list (text — already parsed, but included for completeness):
    "terrdata/theater.lst",

    // Aircraft (sample .dat — small, useful for f4-convert cross-check):
    "sim/F-16.dat",
    "sim/F-18.dat",
};

// -------------------------------------------------------------------------
// File-existence helper — case-insensitive match against the install
// root. Returns the actual on-disk path if found, empty path otherwise.
//
// Falcon4 installs historically mix case: vanilla Falcon 4.0 ships
// uppercase (FALCON4.CT), FreeFalcon ships mixed (Falcon4.PHD), and
// Linux/Wine installs may have any case depending on how the files
// were extracted. We do a directory scan + case-insensitive compare
// rather than relying on the filesystem being case-sensitive.
// -------------------------------------------------------------------------
std::filesystem::path resolve_case_insensitive(
    const std::filesystem::path& search_root,
    const std::filesystem::path& relative_target) {

    std::error_code ec;
    if (!std::filesystem::exists(search_root, ec)) return {};

    // Walk the relative path component-by-component, matching each
    // component case-insensitively against the directory listing.
    std::filesystem::path current = search_root;
    for (const auto& component : relative_target) {
        const std::string target = component.string();
        std::string target_lower;
        target_lower.reserve(target.size());
        for (char c : target) target_lower.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

        bool found = false;
        for (const auto& entry : std::filesystem::directory_iterator(current, ec)) {
            const std::string name = entry.path().filename().string();
            std::string name_lower;
            name_lower.reserve(name.size());
            for (char c : name) name_lower.push_back(
                static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            if (name_lower == target_lower) {
                current = entry.path();
                found = true;
                break;
            }
        }
        if (!found) return {};
    }

    std::error_code exist_ec;
    if (!std::filesystem::is_regular_file(current, exist_ec)) return {};
    return current;
}

// -------------------------------------------------------------------------
// Read up to `max_bytes` bytes from the given file. Returns the bytes
// read (may be fewer than max_bytes if the file is smaller). Sets
// `err_msg` on failure (file not readable, etc.).
// -------------------------------------------------------------------------
std::vector<unsigned char> read_head(const std::filesystem::path& path,
                                      std::size_t max_bytes,
                                      std::string& err_msg) {
    err_msg.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err_msg = "cannot open";
        return {};
    }
    std::vector<unsigned char> buf;
    buf.resize(max_bytes);
    f.read(reinterpret_cast<char*>(buf.data()),
           static_cast<std::streamsize>(buf.size()));
    std::streamsize got = f.gcount();
    if (got < 0) got = 0;
    buf.resize(static_cast<std::size_t>(got));
    return buf;
}

// -------------------------------------------------------------------------
// Read the last `max_bytes` bytes of a file. Returns empty vector on
// failure or if the file is smaller than max_bytes (in which case the
// caller should just use read_head).
// -------------------------------------------------------------------------
std::vector<unsigned char> read_tail(const std::filesystem::path& path,
                                      std::size_t max_bytes,
                                      std::string& err_msg) {
    err_msg.clear();
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) { err_msg = "file_size: " + ec.message(); return {}; }
    if (size <= max_bytes) return {};  // caller should use read_head

    std::ifstream f(path, std::ios::binary);
    if (!f) { err_msg = "cannot open"; return {}; }
    f.seekg(static_cast<std::streamoff>(size - max_bytes), std::ios::beg);
    if (!f) { err_msg = "seek failed"; return {}; }

    std::vector<unsigned char> buf;
    buf.resize(max_bytes);
    f.read(reinterpret_cast<char*>(buf.data()),
           static_cast<std::streamsize>(buf.size()));
    std::streamsize got = f.gcount();
    if (got < 0) got = 0;
    buf.resize(static_cast<std::size_t>(got));
    return buf;
}

// -------------------------------------------------------------------------
// Classic xxd-style hex+ASCII dump of a byte buffer. Each line:
//   00000000  50 48 44 01 00 00 80 00  00 00 00 80 00 00 00 80  |PHD.............|
//
// Offset is 8-hex (zero-padded). 16 bytes per line, with an extra
// space at byte 8 for readability. Non-printable bytes render as '.'
// in the ASCII column. Printable range is 0x20..0x7E inclusive.
// -------------------------------------------------------------------------
void append_hex_dump(std::ostringstream& out,
                     const unsigned char* data, std::size_t len,
                     std::size_t base_offset = 0) {
    constexpr std::size_t kBytesPerLine = 16;
    char line[128];
    for (std::size_t off = 0; off < len; off += kBytesPerLine) {
        const std::size_t line_len = std::min(kBytesPerLine, len - off);
        const std::size_t abs_off = base_offset + off;

        // Offset
        std::snprintf(line, sizeof(line), "%08zx  ", abs_off);
        out << line;

        // Hex bytes — first 8
        for (std::size_t i = 0; i < 8; ++i) {
            if (i < line_len)
                std::snprintf(line, sizeof(line), "%02x ", data[off + i]);
            else
                std::snprintf(line, sizeof(line), "   ");
            out << line;
        }
        out << ' ';  // extra space at byte 8
        // Hex bytes — last 8
        for (std::size_t i = 8; i < 16; ++i) {
            if (i < line_len)
                std::snprintf(line, sizeof(line), "%02x ", data[off + i]);
            else
                std::snprintf(line, sizeof(line), "   ");
            out << line;
        }
        out << ' ';

        // ASCII
        out << '|';
        for (std::size_t i = 0; i < line_len; ++i) {
            const unsigned char c = data[off + i];
            out << static_cast<char>((c >= 0x20 && c <= 0x7e) ? c : '.');
        }
        out << '|';
        out << '\n';
    }
}

// -------------------------------------------------------------------------
// ISO-8601 timestamp for the snapshot header. UTC, second precision —
// good enough for "when was this snapshot taken".
// -------------------------------------------------------------------------
std::string iso_timestamp_now() {
    using std::chrono::system_clock;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

// -------------------------------------------------------------------------
// Build a one-line summary of a file: name + size, or "(absent)".
// Used in the per-theater overview section.
// -------------------------------------------------------------------------
std::string file_summary(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return p.filename().string() + ": (absent)";
    const auto sz = std::filesystem::file_size(p, ec);
    if (ec) return p.filename().string() + ": (error: " + ec.message() + ")";
    return p.filename().string() + ": present (" + std::to_string(sz) + " bytes)";
}

// -------------------------------------------------------------------------
// Recursive file-listing walker. Called when opts.full_recursive_listing
// is set. Walks the entire install root recursively and appends every
// regular file (relative path + size) to `out`, sorted within each
// directory for deterministic output. Symlinks are listed but NOT
// followed (so symlink loops can't crash the walk). Directories are not
// listed themselves but ARE counted in dir_count.
//
// `base` is the install root used to compute the relative path; it does
// not change across the recursion.
//
// Errors (permission denied, etc.) on individual entries are reported
// inline and don't abort the walk — we want to know about every file we
// couldn't read, not crash on the first one.
// -------------------------------------------------------------------------
void walk_and_list(std::ostringstream& out,
                   const std::filesystem::path& dir,
                   const std::filesystem::path& base,
                   std::size_t& file_count,
                   std::size_t& dir_count,
                   std::uintmax_t& total_bytes) {
    namespace fs = std::filesystem;
    std::error_code iter_ec;
    std::vector<fs::path> entries;
    for (auto it = fs::directory_iterator(dir, iter_ec);
         !iter_ec && it != fs::directory_iterator();
         it.increment(iter_ec)) {
        entries.push_back(it->path());
    }
    if (iter_ec) {
        out << "  (error iterating " << dir.string()
            << ": " << iter_ec.message() << ")\n";
        return;
    }
    std::sort(entries.begin(), entries.end());

    constexpr std::size_t kPathColumn = 60;  // align sizes in a column
    for (const auto& entry : entries) {
        std::error_code rel_ec;
        const auto rel = fs::relative(entry, base, rel_ec);
        std::string rel_str = rel_ec ? entry.string() : rel.generic_string();

        std::error_code type_ec;
        const auto status = fs::status(entry, type_ec);
        if (type_ec) {
            out << "  " << rel_str;
            if (rel_str.size() < kPathColumn) {
                out << std::string(kPathColumn - rel_str.size(), ' ');
            } else {
                out << "  ";
            }
            out << "(status error: " << type_ec.message() << ")\n";
            continue;
        }

        if (fs::is_directory(status)) {
            ++dir_count;
            // Recurse. Don't print directory entries themselves — their
            // presence is implicit in the paths of the files they contain.
            walk_and_list(out, entry, base, file_count, dir_count, total_bytes);
        } else if (fs::is_regular_file(status)) {
            ++file_count;
            std::error_code sz_ec;
            const auto sz = fs::file_size(entry, sz_ec);
            out << "  " << rel_str;
            if (rel_str.size() < kPathColumn) {
                out << std::string(kPathColumn - rel_str.size(), ' ');
            } else {
                out << "  ";
            }
            if (sz_ec) {
                out << "(size error: " << sz_ec.message() << ")\n";
            } else {
                total_bytes += sz;
                out << sz << " bytes\n";
            }
        } else if (fs::is_symlink(status)) {
            // Read the link target — but do NOT follow it.
            ++file_count;
            std::error_code link_ec;
            const auto target = fs::read_symlink(entry, link_ec);
            out << "  " << rel_str;
            if (rel_str.size() < kPathColumn) {
                out << std::string(kPathColumn - rel_str.size(), ' ');
            } else {
                out << "  ";
            }
            if (link_ec) {
                out << "(symlink; target unreadable: "
                    << link_ec.message() << ")\n";
            } else {
                out << "(symlink -> " << target.string() << ")\n";
            }
        } else {
            // Other (block/char/fifo/socket) — rare but possible on
            // POSIX. Report it so the user knows something unusual is
            // sitting in the install tree.
            ++file_count;
            out << "  " << rel_str;
            if (rel_str.size() < kPathColumn) {
                out << std::string(kPathColumn - rel_str.size(), ' ');
            } else {
                out << "  ";
            }
            out << "(other file type)\n";
        }
    }
}

// -------------------------------------------------------------------------
// Top-level wrapper around walk_and_list() — emits the section header,
// runs the walk, then emits the section footer with summary counts.
// -------------------------------------------------------------------------
void append_recursive_file_listing(std::ostringstream& out,
                                    const std::filesystem::path& walk_root,
                                    SnapshotResult& result) {
    namespace fs = std::filesystem;
    out << "\n=== FULL RECURSIVE FILE LISTING ===\n";
    out << "walk_root: " << walk_root.string() << "\n";

    std::error_code root_ec;
    if (!fs::exists(walk_root, root_ec)) {
        out << "(walk_root does not exist)\n";
        out << "total_files: 0\n";
        out << "total_dirs: 0\n";
        out << "total_bytes: 0\n";
        return;
    }
    if (!fs::is_directory(walk_root, root_ec)) {
        out << "(walk_root is not a directory)\n";
        out << "total_files: 0\n";
        out << "total_dirs: 0\n";
        out << "total_bytes: 0\n";
        return;
    }

    std::size_t file_count = 0;
    std::size_t dir_count = 0;
    std::uintmax_t total_bytes = 0;
    walk_and_list(out, walk_root, walk_root, file_count, dir_count, total_bytes);

    out << "total_files: " << file_count << "\n";
    out << "total_dirs: " << dir_count << "\n";
    out << "total_bytes: " << total_bytes << "\n";

    result.listed_files = file_count;
    result.listed_dirs = dir_count;
    result.listed_bytes = total_bytes;
}

// -------------------------------------------------------------------------
// Append a single file's snapshot section to `out`.
// -------------------------------------------------------------------------
void append_file_section(std::ostringstream& out,
                          int file_index, int file_count,
                          const std::string& relative_label,
                          const std::filesystem::path& full_path,
                          const SnapshotOptions& opts,
                          std::size_t& dumped_count,
                          std::size_t& missing_count) {
    out << "\n=== FILE " << file_index << "/" << file_count << ": "
        << relative_label << " ===\n";
    out << "path: " << full_path.string() << "\n";

    std::error_code exist_ec;
    if (!std::filesystem::exists(full_path, exist_ec)) {
        out << "status: ABSENT (file not found at this path)\n";
        ++missing_count;
        return;
    }

    std::error_code sz_ec;
    const auto file_size = std::filesystem::file_size(full_path, sz_ec);
    if (sz_ec) {
        out << "status: ERROR (" << sz_ec.message() << ")\n";
        ++missing_count;
        return;
    }

    const std::size_t dump_size = std::min(file_size, opts.per_file_byte_cap);
    out << "size: " << file_size << " bytes  (dumping first "
        << dump_size << " bytes)\n";

    std::string err;
    auto bytes = read_head(full_path, dump_size, err);
    if (!err.empty()) {
        out << "status: READ ERROR (" << err << ")\n";
        ++missing_count;
        return;
    }

    if (bytes.empty()) {
        out << "(empty file — no bytes to dump)\n";
        ++dumped_count;
        return;
    }

    out << "--- hex dump ---\n";
    append_hex_dump(out, bytes.data(), bytes.size(), /*base_offset=*/0);

    // Optional tail dump — useful for files with a trailing sentinel
    // (e.g. Falcon4.PD's terminal record). Skipped if the file is
    // smaller than the per-file cap (we already dumped the whole thing).
    if (opts.include_tail && file_size > opts.per_file_byte_cap) {
        out << "--- tail (" << opts.per_file_byte_cap
            << " bytes from offset " << (file_size - opts.per_file_byte_cap)
            << ") ---\n";
        auto tail = read_tail(full_path, opts.per_file_byte_cap, err);
        if (!err.empty()) {
            out << "(tail read error: " << err << ")\n";
        } else if (!tail.empty()) {
            append_hex_dump(out, tail.data(), tail.size(),
                            /*base_offset=*/file_size - tail.size());
        }
    }

    ++dumped_count;
}

// -------------------------------------------------------------------------
// Append a directory listing section (filenames + sizes only, no hex
// dump). Used for terrdata/objects/, terrdata/ai/, etc. — catches any
// files our curated list missed.
// -------------------------------------------------------------------------
void append_directory_listing(std::ostringstream& out,
                               const std::string& label,
                               const std::filesystem::path& dir) {
    out << "\n--- " << label << " ---\n";
    out << "dir: " << dir.string() << "\n";

    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        out << "(directory absent)\n";
        return;
    }

    std::vector<std::filesystem::path> entries;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        entries.push_back(e.path());
    }
    if (ec) {
        out << "(directory iteration error: " << ec.message() << ")\n";
        return;
    }

    std::sort(entries.begin(), entries.end());
    if (entries.empty()) {
        out << "(empty)\n";
        return;
    }
    for (const auto& e : entries) {
        std::error_code sz_ec;
        std::string size_str;
        if (std::filesystem::is_regular_file(e, sz_ec)) {
            const auto sz = std::filesystem::file_size(e, sz_ec);
            size_str = sz_ec ? "?" : std::to_string(sz);
        } else if (std::filesystem::is_directory(e, sz_ec)) {
            size_str = "<dir>";
        } else {
            size_str = "<other>";
        }
        out << "  " << e.filename().string() << "  (" << size_str << ")\n";
    }
}

} // namespace

SnapshotResult build_install_snapshot(const f4::install::Installation& inst,
                                       const SnapshotOptions& opts) {
    SnapshotResult result;
    std::ostringstream out;

    // --- Header ---
    out << "=== F4 INSTALL SNAPSHOT ===\n";
    out << "generated: " << iso_timestamp_now() << "\n";
    out << "install_root: " << inst.root().string() << "\n";
    out << "install_valid: " << (inst.valid() ? "yes" : "no") << "\n";
    out << "per_file_byte_cap: " << opts.per_file_byte_cap << "\n";
    out << "include_tail: " << (opts.include_tail ? "yes" : "no") << "\n";
    out << "full_recursive_listing: " << (opts.full_recursive_listing ? "yes" : "no") << "\n";
    out << "skip_curated_dumps: " << (opts.skip_curated_dumps ? "yes" : "no") << "\n";
    out << "file_count: " << kCuratedFiles.size() << "\n";

    out << "\n--- Resolved install paths ---\n";
    out << "  class_table:  " << inst.class_table().string() << "\n";
    out << "  aircraft_dir: " << inst.aircraft_dir().string() << "\n";
    out << "  campaign_dir: " << inst.campaign_dir().string() << "\n";
    out << "  terrdata_dir: " << inst.terrdata_dir().string() << "\n";

    // --- Per-theater overview ---
    out << "\n--- Theaters (" << inst.theaters().size() << ") ---\n";
    for (const auto& t : inst.theaters()) {
        out << "  " << t.display_name << " (" << t.key << ")";
        out << (t.complete() ? "" : " [INCOMPLETE]");
        out << "\n";
        out << "    dir: " << t.dir.string() << "\n";
        out << "    " << file_summary(t.theater_map) << "\n";
        out << "    " << file_summary(t.theater_mea) << "\n";
        out << "    " << file_summary(t.theater_o2) << "\n";
        if (!t.theater_ini.empty()) {
            out << "    " << file_summary(t.theater_ini) << "\n";
        }
        if (!t.theater_files.empty()) {
            out << "    all THEATER.* files (" << t.theater_files.size() << "):\n";
            for (const auto& f : t.theater_files) {
                out << "      " << f.filename().string() << "\n";
            }
        }
    }

    // --- Per-campaign overview (paths + sizes only, no hex) ---
    out << "\n--- Campaigns (" << inst.campaigns().size() << ") ---\n";
    for (const auto& c : inst.campaigns()) {
        out << "  " << c.stem;
        if (!c.theater_key.empty()) out << "  [" << c.theater_key << "]";
        out << "\n    " << file_summary(c.cam) << "\n";
    }

    // --- Optional full recursive file listing ---
    // Comes before the curated dumps so the reader gets the "here's
    // everything in this install" overview first, then the deep dives.
    if (opts.full_recursive_listing) {
        append_recursive_file_listing(out, inst.root(), result);
    }

    // --- Curated file dumps ---
    // Skippable via opts.skip_curated_dumps for inventory-only runs.
    if (!opts.skip_curated_dumps) {
        out << "\n=== CURATED FILE DUMPS ===\n";
        int file_idx = 0;
        for (const char* rel : kCuratedFiles) {
            ++file_idx;
            const std::filesystem::path relative(rel);
            const auto resolved = resolve_case_insensitive(inst.root(), relative);
            if (resolved.empty()) {
                // File not found — record it as absent but still emit a
                // section so the user can see we looked for it.
                std::ostringstream dummy;
                std::size_t dummy_dumped = 0, dummy_missing = 0;
                append_file_section(dummy, file_idx, static_cast<int>(kCuratedFiles.size()),
                                    rel, inst.root() / relative, opts,
                                    dummy_dumped, dummy_missing);
                out << dummy.str();
                result.files_missing += dummy_missing;
                result.files_dumped  += dummy_dumped;
            } else {
                append_file_section(out, file_idx, static_cast<int>(kCuratedFiles.size()),
                                    rel, resolved, opts,
                                    result.files_dumped, result.files_missing);
            }
        }
    }

    // --- Optional directory listings (catch files we missed) ---
    // Skip when full_recursive_listing is on — the full walk already
    // covers these directories.
    if (opts.list_terrdata_files && !opts.full_recursive_listing) {
        out << "\n=== DIRECTORY LISTINGS (catch-all) ===\n";
        if (!inst.terrdata_dir().empty()) {
            append_directory_listing(out, "terrdata/",
                                      inst.terrdata_dir());
            append_directory_listing(out, "terrdata/objects/",
                                      inst.terrdata_dir() / "objects");
            append_directory_listing(out, "terrdata/ai/",
                                      inst.terrdata_dir() / "ai");
            append_directory_listing(out, "terrdata/weather/",
                                      inst.terrdata_dir() / "weather");
            append_directory_listing(out, "terrdata/terrain/",
                                      inst.terrdata_dir() / "terrain");
        }
        if (!inst.aircraft_dir().empty()) {
            append_directory_listing(out, "sim/ (aircraft)",
                                      inst.aircraft_dir());
        }
        if (!inst.campaign_dir().empty()) {
            append_directory_listing(out, "campaign/",
                                      inst.campaign_dir());
        }
    }

    // --- Footer ---
    out << "\n=== END OF SNAPSHOT ===\n";
    out << "files_dumped: " << result.files_dumped << "\n";
    out << "files_missing: " << result.files_missing << "\n";
    if (opts.full_recursive_listing) {
        out << "files_listed: " << result.listed_files << "\n";
        out << "dirs_traversed: " << result.listed_dirs << "\n";
        out << "total_bytes_listed: " << result.listed_bytes << "\n";
    }

    result.text = out.str();
    result.total_bytes = result.text.size();
    return result;
}

bool write_install_snapshot(const f4::install::Installation& inst,
                             const std::filesystem::path& output_path,
                             const SnapshotOptions& opts,
                             std::string* err_out) {
    try {
        const auto result = build_install_snapshot(inst, opts);
        std::ofstream f(output_path, std::ios::binary);
        if (!f) {
            if (err_out) *err_out = "cannot open output file: " + output_path.string();
            return false;
        }
        f.write(result.text.data(),
                static_cast<std::streamsize>(result.text.size()));
        if (!f) {
            if (err_out) *err_out = "write failed: " + output_path.string();
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        if (err_out) *err_out = std::string("exception: ") + e.what();
        return false;
    }
}

} // namespace f4::viewer
