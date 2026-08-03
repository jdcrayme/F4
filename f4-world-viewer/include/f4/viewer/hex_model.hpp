// f4-world-viewer/include/f4/viewer/hex_model.hpp
//
// Pure data model for the Hex Inspector panel. No raylib, no ImGui —
// just bytes, ranges, and decoder annotations. This separation lets us
// unit-test the decoders and range logic without spinning up a window.
//
// Usage:
//   HexModel m;
//   m.load_file("/path/to/save1.cam");        // memory-maps (or reads) the file
//   m.apply_decoder(identify_file(m.path())); // populate annotations
//
//   // Or, for testing without a real file:
//   HexModel m;
//   m.load_bytes("test.cam", {0x4D, 0x41, 0x50, ...});
//
//   // Iterate:
//   for (const auto& ann : m.annotations()) {
//       std::cout << ann.range.offset << "+" << ann.range.length
//                 << ": " << ann.label << " = " << ann.value << "\n";
//   }
//
// The Hex Inspector panel (hex_inspector.hpp) renders this model with
// ImGui; the panel itself is a view, all the interesting logic is here.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace f4::viewer {

/// A byte range within the loaded file. Half-open [offset, offset+length).
struct ByteRange {
    std::size_t offset = 0;
    std::size_t length = 0;

    [[nodiscard]] bool empty() const noexcept { return length == 0; }
    [[nodiscard]] std::size_t end() const noexcept { return offset + length; }

    /// True if `pos` falls within this range.
    [[nodiscard]] bool contains(std::size_t pos) const noexcept {
        return pos >= offset && pos < end();
    }

    /// True if this range overlaps with `other`.
    [[nodiscard]] bool overlaps(ByteRange other) const noexcept {
        return offset < other.end() && other.offset < end();
    }

    bool operator==(const ByteRange&) const = default;
};

/// One decoder annotation. Describes what a particular byte range means
/// in the context of the file's format. The Hex Inspector draws these
/// as colored overlays on the hex dump + ASCII view.
struct Annotation {
    ByteRange range;
    std::string label;         // short name, e.g. "magic", "num_subfiles", "team_name[0]"
    std::string value;         // human-readable value, e.g. "0x444CFFAE", "10", "U.S."
    std::string description;   // longer explanation, shown in the inspector panel
    std::string category;      // for color coding: "header", "field", "string", "padding", "unknown"

    bool operator==(const Annotation&) const = default;
};

/// File type identification — used to pick the right decoder.
enum class FileType {
    Unknown,
    CamArchive,      // .cam — campaign save container
    CmpSubfile,      // .cmp — campaign metadata sub-file (LZSS-compressed)
    TheaterMap,      // THEATER.MAP — terrain header + palette
    TheaterMea,      // THEATER.MEA — elevation grid
    TheaterO2,       // THEATER.O2 — secondary overlay
    Falcon4Ct,       // FALCON4.ct — class table
    Dat,             // .dat — aircraft definition (legacy text format)
    Ver,             // .ver — version text file
    Lua,             // .lua — script
    Text,            // generic text
    Binary,          // unknown binary
};

[[nodiscard]] const char* file_type_name(FileType t) noexcept;

/// Identify a file's type by extension + magic bytes. Used by the Hex
/// Inspector to pick the right decoder automatically.
[[nodiscard]] FileType identify_file(const std::filesystem::path& path,
                                       const uint8_t* data,
                                       std::size_t size);

/// Convenience: identify by path alone (no magic byte probe). Less
/// accurate but useful when we don't want to read the file yet.
[[nodiscard]] FileType identify_file_by_extension(const std::filesystem::path& path);

/// The Hex Inspector's data model. Owns the loaded file's bytes and the
/// list of decoder annotations. Cheap to copy the metadata; the byte
/// buffer is shared via a shared_ptr to avoid copying large files.
class HexModel {
public:
    HexModel() = default;

    /// Load a file from disk into memory. Throws on I/O error.
    /// For files > 64 MB we still load fully (memory is cheap; the
    /// inspector is a dev tool, not a production path). Future: mmap.
    void load_file(const std::filesystem::path& path);

    /// Load bytes from memory (for testing or for already-loaded data).
    /// `path` is stored as metadata but no file is read.
    void load_bytes(const std::filesystem::path& path,
                     const std::vector<uint8_t>& bytes);

    /// Load bytes from memory (move overload — avoids a copy for large fixtures).
    void load_bytes(const std::filesystem::path& path,
                     std::vector<uint8_t>&& bytes);

    /// Apply the appropriate decoder for the loaded file's type. Clears
    /// any existing annotations first. If the type is Unknown, runs the
    /// generic decoder (size / magic / entropy / ASCII strings).
    void apply_decoder();

    /// Apply a specific decoder regardless of the detected type. Useful
    /// when the user wants to force interpretation (e.g. inspect a .cmp
    /// sub-file extracted from a .cam as a standalone file).
    void apply_decoder(FileType type);

    /// Manually set the annotations (for testing or for custom decoders).
    void set_annotations(std::vector<Annotation> anns) { annotations_ = std::move(anns); }

    // --- Accessors ---

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] const std::vector<uint8_t>& bytes() const noexcept { return bytes_; }
    [[nodiscard]] const std::vector<Annotation>& annotations() const noexcept { return annotations_; }
    [[nodiscard]] FileType file_type() const noexcept { return file_type_; }

    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] bool loaded() const noexcept { return !bytes_.empty(); }

    /// Get a single byte. Returns 0 if out of range.
    [[nodiscard]] uint8_t byte_at(std::size_t offset) const noexcept {
        return offset < bytes_.size() ? bytes_[offset] : 0;
    }

    /// Get a slice of bytes [offset, offset+length). Bounds-clamped.
    [[nodiscard]] std::vector<uint8_t> slice(std::size_t offset, std::size_t length) const;

    /// Read up to 8 bytes as a little-endian unsigned integer. `length`
    /// must be 1-8. Returns 0 if out of range.
    [[nodiscard]] uint64_t read_le(std::size_t offset, std::size_t length) const noexcept;

    /// Read a fixed-length null-terminated string from `offset`. The
    /// string is at most `max_len` bytes (trailing bytes after the null
    /// are skipped). Used by decoders for fixed-width char[N] fields.
    [[nodiscard]] std::string read_fixed_string(std::size_t offset,
                                                  std::size_t max_len) const noexcept;

    // --- Selection ---

    [[nodiscard]] ByteRange selection() const noexcept { return selection_; }
    void set_selection(ByteRange r) noexcept { selection_ = r; }
    void clear_selection() noexcept { selection_ = {}; }

    /// Find the annotation (if any) that contains `offset`. Returns
    /// nullptr if none. Used by the inspector panel to show the user
    /// what they're hovering over.
    [[nodiscard]] const Annotation* annotation_at(std::size_t offset) const noexcept;

    /// Compute a cheap entropy estimate (0-8 bits/byte) for the loaded
    /// bytes. Used by the generic decoder to give a hint about whether
    /// the file is compressed / encrypted / plaintext.
    [[nodiscard]] double entropy() const noexcept;

    /// Extract printable ASCII runs of at least `min_length` characters.
    /// Used by the generic decoder to surface strings in unknown binary files.
    [[nodiscard]] std::vector<ByteRange> find_ascii_strings(std::size_t min_length = 4) const;

private:
    std::filesystem::path path_;
    std::vector<uint8_t> bytes_;
    std::vector<Annotation> annotations_;
    FileType file_type_ = FileType::Unknown;
    ByteRange selection_;
};

} // namespace f4::viewer
