// f4-convert/cli/brain2json.cpp
//
// CLI tool: convert a FreeFalcon .brn DigitalBrain archetype file
// (BRAINDAT.brn or GENERIC.BRN) to f4 JSON format.
//
// Usage:
//   brain2json <input.brn> <output.json>
//
// Exit codes:
//   0  success
//   2  parse failure
//   3  write failure

#include "f4/convert/brain_parser.hpp"
#include "f4/data/brain_data.hpp"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: %s <input.brn> <output.json>\n",
                     argv[0]);
        return 1;
    }

    const std::string inputPath = argv[1];
    const std::string outputPath = argv[2];
    std::printf("Converting %s -> %s\n", inputPath.c_str(), outputPath.c_str());

    auto result = f4::convert::loadBrainFile(inputPath);
    if (!result.ok) {
        std::fprintf(stderr, "ERROR: failed to parse %s\n", inputPath.c_str());
        for (auto const& e : result.errors)
            std::fprintf(stderr, "  %s\n", e.c_str());
        return 2;
    }
    for (auto const& w : result.warnings)
        std::printf("WARNING: %s\n", w.c_str());

    if (!f4::data::writeBrainDataFile(result.data, outputPath)) {
        std::fprintf(stderr, "ERROR: failed to write %s\n", outputPath.c_str());
        return 3;
    }

    std::printf("Archetypes: %zu\n", result.data.archetypes.size());
    for (const auto& a : result.data.archetypes) {
        std::printf("  %-12s %zu mode rows\n", a.name.c_str(),
                    a.modes.size());
    }
    if (result.max_gs != 0.0) {
        std::printf("Max Gs trailer: %.1f\n", result.max_gs);
    }
    return 0;
}
