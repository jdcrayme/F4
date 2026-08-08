// f4-convert/cli/dat2json.cpp
//
// CLI tool: convert a Falcon 4 .dat aircraft file to f4 JSON format.
//
// Usage:
//   dat2json <input.dat> <output.json>
//
// Exit codes:
//   0  success
//   2  parse failure (real error)
//   3  write failure
//   4  skipped (AFM format — BMS Advanced Flight Model not supported)

#include "f4/convert/dat_parser.hpp"
#include "f4/convert/json_io.hpp"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: %s <input.dat> <output.json>\n", argv[0]);
        return 1;
    }

    const std::string inputPath  = argv[1];
    const std::string outputPath = argv[2];

    std::printf("Converting %s -> %s\n", inputPath.c_str(), outputPath.c_str());

    auto result = f4::convert::loadFile(inputPath);
    if (!result.ok) {
        // Check for the recognizable AFM-skip message and exit with a
        // distinct code so the bulk-conversion script can count AFM skips
        // separately from real failures.
        bool isAfm = !result.errors.empty() &&
                     result.errors[0].find("AFM format not supported") != std::string::npos;
        if (isAfm) {
            std::fprintf(stderr, "SKIP (AFM): %s\n", inputPath.c_str());
            return 4;  // distinct exit code for AFM skips
        }
        std::fprintf(stderr, "ERROR: Failed to parse %s\n", inputPath.c_str());
        for (auto const& e : result.errors) std::fprintf(stderr, "  %s\n", e.c_str());
        return 2;
    }

    if (!result.warnings.empty()) {
        std::printf("Warnings (%zu):\n", result.warnings.size());
        for (auto const& w : result.warnings) std::printf("  %s\n", w.c_str());
    }

    if (!f4::convert::writeJsonFile(result.config, outputPath)) {
        std::fprintf(stderr, "ERROR: Failed to write %s\n", outputPath.c_str());
        return 3;
    }

    auto const& c = result.config;
    std::printf("\nConverted aircraft: %s\n", c.name.c_str());
    std::printf("  Empty weight:    %.1f lbs\n", c.geometry.emptyWeight.value());
    std::printf("  Wing area:       %.1f ft^2\n", c.geometry.area.value());
    std::printf("  Internal fuel:   %.1f lbs\n", c.geometry.internalFuel.value());
    std::printf("  Span:            %.1f ft\n", c.geometry.span.value());
    std::printf("  Max Gs:          %.1f\n", c.geometry.maxGs);
    std::printf("  AOA limits:      %.1f to %.1f deg\n",
                c.geometry.aoaMin.to<f4::Degrees>().value(), c.geometry.aoaMax.to<f4::Degrees>().value());
    std::printf("  Gear points:     %zu\n", c.geometry.gear.size());
    std::printf("  Engines:         %d (type %d)\n", c.aux.nEngines, c.aux.typeEngine);
    std::printf("  Aero table:      %zu mach x %zu alpha\n",
                c.aero.mach.size(), c.aero.alpha_deg.size());
    std::printf("  Engine table:    %zu alt x %zu mach\n",
                c.engine.alt_ft.size(), c.engine.mach.size());
    std::printf("  Roll table:      %zu alpha x %zu qbar\n",
                c.rollCmd.alpha_deg.size(), c.rollCmd.qbar.size());
    std::printf("  Has AB:          %s\n", c.engine.hasAB() ? "yes" : "no");
    std::printf("  rawAuxAeroData:  %zu keys captured\n", c.rawAuxAeroData.size());

    if (!c.engine.thrust_mil.empty() && !c.engine.thrust_ab.empty()) {
        std::printf("  Sea-level MIL:   %.0f lbf\n", c.engine.thrust_mil[0]);
        std::printf("  Sea-level AB:    %.0f lbf\n", c.engine.thrust_ab[0]);
    }

    std::printf("\nWrote %s\n", outputPath.c_str());
    return 0;
}
