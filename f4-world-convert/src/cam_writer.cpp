// f4-world-convert/src/cam_writer.cpp
//
// CamWriter — assembles a .cam archive from sub-files. The inverse of
// CamArchive::load (cam_archive.cpp). See cam_writer.hpp for the format.

#include <f4/world_convert/cam_writer.hpp>
#include <f4/json/reader.hpp>
#include "byte_writer.hpp"

#include <fstream>
#include <stdexcept>

namespace f4::world_convert {

namespace {

// base64 decoder — the inverse of world_json.cpp's base64_encode.
std::vector<uint8_t> base64_decode(const std::string& s) {
    static constexpr int8_t kInvalid = -1;
    auto val = [](char c) -> int8_t {
        if (c >= 'A' && c <= 'Z') return static_cast<int8_t>(c - 'A');
        if (c >= 'a' && c <= 'z') return static_cast<int8_t>(c - 'a' + 26);
        if (c >= '0' && c <= '9') return static_cast<int8_t>(c - '0' + 52);
        if (c == '+') return 62;
        if (c == '/') return 63;
        return kInvalid;
    };
    std::vector<uint8_t> out;
    out.reserve((s.size() / 4) * 3);
    int buf = 0, bits = 0;
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        if (c == '=') break;   // padding terminates the data
        const int8_t v = val(c);
        if (v < 0) throw std::runtime_error("cam_from_world_json: invalid base64 character");
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// Parse the "subfiles_b64" array and feed each sub-file into the writer.
void parse_subfiles_b64(f4::json::Reader& r, CamWriter& w) {
    r.expect('[');
    if (r.consume(']')) return;   // empty array
    for (;;) {
        r.expect('{');
        if (!r.consume('}')) {    // non-empty object
            std::string name;
            std::string data_b64;
            bool have_data = false;
            for (;;) {
                std::string key = r.read_string();
                r.expect(':');
                if (key == "name") {
                    name = r.read_string();
                } else if (key == "data_b64") {
                    data_b64 = r.read_string();
                    have_data = true;
                } else {
                    r.skip_value();
                }
                if (r.consume('}')) break;
                r.expect(',');
            }
            if (!have_data) {
                throw std::runtime_error(
                    "cam_from_world_json: subfiles_b64 entry missing data_b64");
            }
            w.add(name, base64_decode(data_b64));
        }
        if (r.consume(']')) break;
        r.expect(',');
    }
}

} // namespace

void CamWriter::add(std::string name, std::vector<uint8_t> data) {
    subfiles_.push_back(CamSubfileInput{std::move(name), std::move(data)});
}

void CamWriter::add(std::string name, const uint8_t* data, std::size_t size) {
    CamSubfileInput sf;
    sf.name = std::move(name);
    sf.data.assign(data, data + size);
    subfiles_.push_back(std::move(sf));
}

std::vector<uint8_t> CamWriter::build() const {
    // Layout: [0..3] manifest_offset; [4..4+total) sub-file data; then the
    // manifest (int32 count + per-subfile directory entries).
    ByteWriter w;

    // Reserve the 4-byte manifest_offset placeholder; fill it after we
    // know the total data size.
    w.i32(0);   // placeholder for manifest_offset

    // Write the concatenated sub-file data, recording each one's absolute
    // offset (data starts at byte 4).
    struct Entry { std::string name; int32_t offset; int32_t size; };
    std::vector<Entry> entries;
    entries.reserve(subfiles_.size());

    int32_t offset = 4;   // first sub-file starts right after the header
    for (const auto& sf : subfiles_) {
        entries.push_back(Entry{sf.name, offset, static_cast<int32_t>(sf.data.size())});
        w.bytes(sf.data.data(), sf.data.size());
        offset += static_cast<int32_t>(sf.data.size());
    }

    const int32_t manifest_offset = offset;

    // Manifest directory.
    w.i32(static_cast<int32_t>(subfiles_.size()));
    for (const auto& e : entries) {
        // name_len as a single byte (names are short, e.g. "save1.cmp").
        // CamArchive::load rejects names that overrun the buffer; cap at 255.
        const std::size_t name_len =
            std::min<std::size_t>(e.name.size(), 255);
        w.u8(static_cast<uint8_t>(name_len));
        w.bytes(e.name.data(), name_len);
        w.i32(e.offset);
        w.i32(e.size);
    }

    // Patch the manifest_offset placeholder at [0..3].
    std::memcpy(w.buf.data(), &manifest_offset, 4);

    return w.buf;
}

void CamWriter::write(const std::filesystem::path& path) const {
    auto bytes = build();
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("CamWriter: cannot write " + path.string());
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    if (!f) throw std::runtime_error("CamWriter: short write to " + path.string());
}

std::vector<uint8_t> cam_from_world_json(const std::string& world_json) {
    f4::json::Reader r(world_json);
    r.skip_ws();
    r.expect('{');

    CamWriter w;
    bool found = false;
    if (!r.consume('}')) {    // non-empty top-level object
        for (;;) {
            std::string key = r.read_string();
            r.expect(':');
            if (key == "subfiles_b64") {
                parse_subfiles_b64(r, w);
                found = true;
            } else {
                r.skip_value();
            }
            if (r.consume('}')) break;
            r.expect(',');
        }
    }
    if (!found) {
        throw std::runtime_error(
            "cam_from_world_json: no \"subfiles_b64\" block in world JSON "
            "(produce one with `cam2json --preserve-subfiles`)");
    }
    return w.build();
}

} // namespace f4::world_convert
