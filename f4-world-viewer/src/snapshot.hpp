// f4-world-viewer/src/snapshot.hpp
//
// PRIVATE HEADER — internal to the f4-world-viewer library. Declares the
// snapshot-builder free function used by:
//   - ViewerApp::open_snapshot_dialog (Tools > Snapshot Install Files...)
//   - ViewerApp::snapshot_install_files (the --snapshot CLI flag)
//
// WHAT THIS IS FOR
// ----------------
// The Falcon4 static-data parsers we still need to write (Falcon4.PHD,
// Falcon4.PD, Falcon4.OCD, Falcon4.UCD, Falcon4.VCD, Falcon4.FED,
// Falcon4.FCD, Falcon4.AII) are documented in the FreeFalcon source by
// C-struct definitions whose on-disk layout depends on MSVC packing,
// hidden padding, and 14 years of upstream drift. The fastest way to
// ground-truth those layouts is to look at real bytes from a real
// install.
//
// Rather than ask the user to mail us individual binary files (which
// is awkward — the user shouldn't have to know which files matter),
// this snapshot tool walks the install they've already pointed the
// viewer at, dumps the first N bytes of every interesting file as a
// classic hex+ASCII dump, and writes it all to ONE plain-text file
// the user can email / attach.
//
// The dump is plain ASCII so it survives any email gateway, never
// contains anything but bytes that were already on disk (no PII), and
// is small enough to attach: with the default 8 KB per-file cap and
// ~15 target files, the snapshot is ~500 KB of text.
//
// SNAPSHOT FILE FORMAT
// --------------------
//   === F4 INSTALL SNAPSHOT ===
//   generated: 2026-08-04T12:34:56Z
//   install_root: C:/Falcon4
//   install_valid: yes
//   per_file_byte_cap: 8192
//   file_count: 14
//
//   --- Theater: korea (Korea) ---
//     dir: C:/Falcon4/terrdata/korea
//     THEATER.MAP: present (1024 bytes)
//     THEATER.MEA: present (8388608 bytes)
//     ...
//
//   [optional, when opts.full_recursive_listing == true]
//   === FULL RECURSIVE FILE LISTING ===
//   walk_root: C:/Falcon4
//   total_files: 1247
//   total_dirs: 87
//   total_bytes: 4567890123
//     FALCON4.CT                                                 12345 bytes
//     sim/F-16.dat                                                8192 bytes
//     sim/F-18.dat                                                8192 bytes
//     terrdata/objects/Falcon4.OCD                               73728 bytes
//     terrdata/korea/THEATER.MAP                                 65536 bytes
//     ...
//
//   [skipped when opts.skip_curated_dumps == true]
//   === FILE 1/14: terrdata/objects/Falcon4.PHD ===
//   path: C:/Falcon4/terrdata/objects/Falcon4.PHD
//   size: 5120 bytes  (dumping first 5120 bytes)
//   00000000  50 48 44 01 00 00 80 00  ...                      |PHD.....|
//   00000010  ...
//   ...
//
//   === FILE 2/14: ... ===
//   ...
//
// The hex dump format is the classic xxd-style:
//   8-hex offset | 16 hex bytes (space-separated, with extra space at byte 8)
//   | 16 ASCII chars (non-printable rendered as '.') |
//
// The recursive listing uses forward slashes for the relative path
// (cross-platform) and is sorted lexicographically within each directory
// for deterministic output. Symlinks are reported as "(symlink -> target)"
// and NOT followed (so symlink loops can't crash the walk). Directory
// entries are not listed themselves (their presence is implicit in the
// files they contain) but ARE counted in total_dirs.

#pragma once

#include <f4/install/installation.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace f4::viewer {

/// Options controlling build_install_snapshot().
struct SnapshotOptions {
    /// Maximum number of bytes to dump per file. The snapshot file grows
    /// roughly 4x this number (each byte becomes ~4 chars of hex+ASCII
    /// plus offsets and whitespace). Default 8192 captures the full
    /// header region of every static-data file we care about (the
    /// largest header we know of is ObjClassDataType at ~560 bytes/entry
    /// × ~10 entries; well under 8 KB). Set to 0 to dump the entire
    /// file (NOT recommended for Falcon4.PD which can be >1 MB).
    std::size_t per_file_byte_cap = 8192;

    /// If true, also dump the last `per_file_byte_cap` bytes of each file
    /// (useful for files with a trailing summary record, like Falcon4.PD's
    /// sentinel). Off by default to keep the snapshot small.
    bool include_tail = false;

    /// If true, walk the install's terrdata/objects/ and terrdata/ai/
    /// directories and list (but don't dump) every file we find — so
    /// we can spot any files our curated list missed. On by default;
    /// the listing is tiny (just filenames + sizes).
    bool list_terrdata_files = true;

    /// If true, walk the ENTIRE install root recursively and emit a
    /// "FULL RECURSIVE FILE LISTING" section that lists every regular
    /// file (relative path + size in bytes). The listing uses forward
    /// slashes for cross-platform readability and is sorted within each
    /// directory for deterministic output.
    ///
    /// This is the canonical "tell me everything that's in this install"
    /// flag — used to document install layouts across vanilla / FreeFalcon
    /// / BMS installs and to spot files our curated list missed. Off by
    /// default to keep the snapshot small on installs with thousands of
    /// files. Enable via the --list-files CLI flag or the "List All
    /// Install Files..." menu item.
    bool full_recursive_listing = false;

    /// If true, skip the curated file hex-dumps entirely and emit ONLY
    /// the header + theater/campaign overview + (optional) full recursive
    /// listing. Useful when the user just wants the file inventory (e.g.
    /// to compare two installs side-by-side) without the multi-MB hex
    /// dump. Pair with full_recursive_listing=true to get a clean
    /// inventory-only snapshot.
    bool skip_curated_dumps = false;
};

/// Result of build_install_snapshot(): the snapshot text plus a summary
/// struct the caller can show in a status bar.
struct SnapshotResult {
    /// The full snapshot text. Plain ASCII, suitable for writing to a
    /// .txt file and emailing.
    std::string text;

    /// Number of files actually dumped. May be less than the curated
    /// target list if some files are absent from the install.
    std::size_t files_dumped = 0;

    /// Number of files in the curated list that were NOT found.
    std::size_t files_missing = 0;

    /// Total bytes of snapshot text (== text.size()).
    std::size_t total_bytes = 0;

    /// Number of regular files enumerated by the full recursive listing
    /// (0 unless opts.full_recursive_listing was true).
    std::size_t listed_files = 0;

    /// Number of directories traversed by the full recursive listing
    /// (0 unless opts.full_recursive_listing was true).
    std::size_t listed_dirs = 0;

    /// Total bytes of all regular files enumerated by the recursive
    /// listing (0 unless opts.full_recursive_listing was true). Useful
    /// as a sanity check against the install's reported disk usage.
    std::uintmax_t listed_bytes = 0;
};

/// Build a snapshot of the given Falcon 4.0 installation.
///
/// Walks the install's well-known directories, dumps the first
/// `opts.per_file_byte_cap` bytes of each curated target file as a
/// hex+ASCII dump, and returns the whole thing as a single ASCII
/// string suitable for writing to a .txt file.
///
/// Never throws — missing files, permission errors, etc. are reported
/// inline in the snapshot text and counted in `files_missing`. The
/// caller can write the returned text directly to disk.
[[nodiscard]] SnapshotResult build_install_snapshot(
    const f4::install::Installation& inst,
    const SnapshotOptions& opts = {});

/// Write a snapshot to the given path. Convenience wrapper around
/// build_install_snapshot() + std::ofstream. Returns true on success.
/// On failure, sets `err_out` (if non-null) to the error message.
bool write_install_snapshot(const f4::install::Installation& inst,
                             const std::filesystem::path& output_path,
                             const SnapshotOptions& opts = {},
                             std::string* err_out = nullptr);

} // namespace f4::viewer
