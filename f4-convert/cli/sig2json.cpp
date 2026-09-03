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

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: %s <sigdata-dir> <output.json>\n",
                     argv[0]);
        return 1;
    }

    const std::string inputDir = argv[1];
    const std::string outputPath = argv[2];
    std::printf("Converting %s -> %s\n", inputDir.c_str(), outputPath.c_str());

    auto result = f4::convert::loadSignatureDataDir(inputDir);
    if (!result.ok) {
        std::fprintf(stderr, "ERROR: failed to parse %s\n", inputDir.c_str());
        for (auto const& e : result.errors)
            std::fprintf(stderr, "  %s\n", e.c_str());
        return 2;
    }
    for (auto const& w : result.warnings)
        std::printf("WARNING: %s\n", w.c_str());

    if (!f4::data::writeSignatureDataLibraryFile(result.library,
                                                 outputPath)) {
        std::fprintf(stderr, "ERROR: failed to write %s\n", outputPath.c_str());
        return 3;
    }

    std::printf("Signature sets: %zu\n", result.library.entries.size());
    for (const auto& e : result.library.entries) {
        std::printf("  %-10s rcs %zux%zu  ir0 %zux%zu  visual %zux%zu\n",
                    e.name.c_str(), e.rcs.values.size(),
                    e.rcs.azimuth_deg.size(), e.ir0.values.size(),
                    e.ir0.azimuth_deg.size(), e.visual.values.size(),
                    e.visual.azimuth_deg.size());
    }
    return 0;
}
