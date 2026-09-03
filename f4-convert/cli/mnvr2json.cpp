// f4-convert/cli/mnvr2json.cpp
//
// CLI tool: convert a FreeFalcon mnvrdata.dat (AI maneuver tables) to
// f4 JSON format.
//
// Usage:
//   mnvr2json <input.dat> <output.json>
//
// Exit codes:
//   0  success
//   2  parse failure

#include "f4/convert/mnvr_parser.hpp"
#include "f4/data/maneuver_data.hpp"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: %s <mnvrdata.dat> <output.json>\n",
                     argv[0]);
        return 1;
    }

    const std::string inputPath = argv[1];
    const std::string outputPath = argv[2];
    std::printf("Converting %s -> %s\n", inputPath.c_str(), outputPath.c_str());

    auto result = f4::convert::loadMnvFile(inputPath);
    if (!result.ok) {
        std::fprintf(stderr, "ERROR: failed to parse %s\n", inputPath.c_str());
        for (auto const& e : result.errors)
            std::fprintf(stderr, "  %s\n", e.c_str());
        return 2;
    }
    for (auto const& w : result.warnings)
        std::printf("WARNING: %s\n", w.c_str());

    if (!f4::data::writeManeuverDataFile(result.data, outputPath)) {
        std::fprintf(stderr, "ERROR: failed to write %s\n", outputPath.c_str());
        return 3;
    }

    std::printf("Maneuver classes:      9\n");
    std::printf("Populated 9x9 cells:   %zu\n",
                result.data.populatedCells());
    for (std::size_t i = 0; i < f4::data::kNumMnvrClasses; ++i) {
        std::printf("  %-8s flags 0x%X\n", f4::data::kMnvrClassNames[i],
                    result.data.classFlags[i]);
    }
    return 0;
}
