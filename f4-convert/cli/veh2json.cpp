// f4-convert/cli/veh2json.cpp
//
// CLI tool: convert a FreeFalcon Vehicle.lst (+ every .veh it
// references, resolved in the same directory) to f4 JSON format.
//
// Usage:
//   veh2json <Vehicle.lst> <output.json>
//
// Exit codes:
//   0  success
//   2  parse failure

#include "f4/convert/veh_parser.hpp"
#include "f4/data/vehicle_def_data.hpp"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr,
                     "Usage: %s <Vehicle.lst> <output.json>\n",
                     argv[0]);
        return 1;
    }

    const std::string inputPath = argv[1];
    const std::string outputPath = argv[2];
    std::printf("Converting %s -> %s\n", inputPath.c_str(), outputPath.c_str());

    auto result = f4::convert::loadVehicleLstFile(inputPath);
    if (!result.ok) {
        std::fprintf(stderr, "ERROR: failed to parse %s\n", inputPath.c_str());
        for (auto const& e : result.errors)
            std::fprintf(stderr, "  %s\n", e.c_str());
        return 2;
    }
    for (auto const& w : result.warnings)
        std::printf("WARNING: %s\n", w.c_str());

    if (!f4::data::writeVehicleDefinitionLibraryFile(result.library,
                                                      outputPath)) {
        std::fprintf(stderr, "ERROR: failed to write %s\n", outputPath.c_str());
        return 3;
    }

    std::printf("Vehicle class rows:  %zu\n", result.library.entries.size());
    for (int t = 0; t <= 4; ++t) {
        std::printf("  %-10s %zu\n", f4::data::kMoverTypeNames[t],
                    result.library.count_of_type(
                        static_cast<f4::data::MoverType>(t)));
    }
    return 0;
}
