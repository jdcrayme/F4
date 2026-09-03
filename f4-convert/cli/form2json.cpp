// f4-convert/cli/form2json.cpp
//
// CLI tool: convert a FreeFalcon FORMDAT.FIL (AI formation geometry) to
// f4 JSON format.
//
// Usage:
//   form2json <FORMDAT.FIL> <output.json>
//
// Exit codes:
//   0  success
//   2  parse failure
//   3  write failure

#include "f4/convert/formation_parser.hpp"
#include "f4/data/formation_data.hpp"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: %s <FORMDAT.FIL> <output.json>\n",
                     argv[0]);
        return 1;
    }

    const std::string inputPath = argv[1];
    const std::string outputPath = argv[2];
    std::printf("Converting %s -> %s\n", inputPath.c_str(), outputPath.c_str());

    auto result = f4::convert::loadFormFile(inputPath);
    if (!result.ok) {
        std::fprintf(stderr, "ERROR: failed to parse %s\n", inputPath.c_str());
        for (auto const& e : result.errors)
            std::fprintf(stderr, "  %s\n", e.c_str());
        return 2;
    }
    for (auto const& w : result.warnings)
        std::printf("WARNING: %s\n", w.c_str());

    if (!f4::data::writeFormationLibraryFile(result.data, outputPath)) {
        std::fprintf(stderr, "ERROR: failed to write %s\n", outputPath.c_str());
        return 3;
    }

    std::printf("Formations: %zu\n", result.data.formations.size());
    for (const auto& f : result.data.formations) {
        std::printf("  %-10s formNum %d, %zu slots, 2-ship az %.1f deg / "
                    "%.3f NM%s\n",
                    f.name.c_str(), f.form_num, f.slots.size(),
                    f.two_ship.rel_az_deg, f.two_ship.range_nm,
                    f.two_ship_explicit ? " (explicit)" : " (from slot 0)");
    }
    return 0;
}
