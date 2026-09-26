// f4-convert/cli/sig2json.cpp
//
// CLI tool: convert a FreeFalcon SIGDATA directory (SIGDATA.LST +
// RCSDAT/, IR/, VISUAL/ grids) to f4 JSON format.
//
// Usage:
//   sig2json <sigdata-dir> <output.json>
//
// Exit codes:
//   0  success
//   2  parse failure

#include "f4/convert/signature_parser.hpp"
#include "f4/data/signature_data.hpp"

#include "f4/convert/simple_convert_cli.hpp"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    return f4::convert::run_simple_convert(
        argc, argv,
        /*usage_args=*/"<sigdata-dir> <output.json>",
        /*load=*/[](const std::string& in) {
            return f4::convert::loadSignatureDataDir(in);
        },
        /*write=*/[](const auto& r, const std::string& output_path) {
            return f4::data::writeSignatureDataLibraryFile(r.library, output_path);
        },
        /*summarize=*/[](const auto& r) {
        std::printf("Signature sets: %zu\n", r.library.entries.size());
        for (const auto& e : r.library.entries) {
        std::printf("  %-10s rcs %zux%zu  ir0 %zux%zu  visual %zux%zu\n",
        e.name.c_str(), e.rcs.values.size(),
        e.rcs.azimuth_deg.size(), e.ir0.values.size(),
        e.ir0.azimuth_deg.size(), e.visual.values.size(),
        e.visual.azimuth_deg.size());
        }
        });
}
