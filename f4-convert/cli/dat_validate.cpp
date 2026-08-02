// f4-convert/cli/dat_validate.cpp
//
// CLI tool: validate a Falcon 4 .dat aircraft file by parsing it and
// reporting any errors or warnings. Does not write any output file.
//
// Usage:
//   dat_validate <input.dat>
//
// Exit codes:
//   0  file parses cleanly (may have warnings)
//   2  parse failure
//   4  skipped (AFM format)

#include "f4/convert/dat_parser.hpp"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s <input.dat>\n", argv[0]);
        return 1;
    }

    const std::string inputPath = argv[1];
    auto result = f4::convert::loadFile(inputPath);

    if (!result.ok) {
        bool isAfm = !result.errors.empty() &&
                     result.errors[0].find("AFM format not supported") != std::string::npos;
        if (isAfm) {
            std::fprintf(stderr, "SKIP (AFM): %s\n", inputPath.c_str());
            return 4;
        }
        std::fprintf(stderr, "FAIL: %s\n", inputPath.c_str());
        for (auto const& e : result.errors) std::fprintf(stderr, "  ERROR: %s\n", e.c_str());
        return 2;
    }

    std::printf("OK: %s\n", inputPath.c_str());
    if (!result.warnings.empty()) {
        std::printf("Warnings (%zu):\n", result.warnings.size());
        for (auto const& w : result.warnings) std::printf("  %s\n", w.c_str());
    }
    auto const& c = result.config;
    std::printf("  Aircraft:        %s\n", c.name.c_str());
    std::printf("  Aero table:      %zu mach x %zu alpha\n",
                c.aero.mach.size(), c.aero.alpha_deg.size());
    std::printf("  Engine table:    %zu alt x %zu mach\n",
                c.engine.alt_ft.size(), c.engine.mach.size());
    std::printf("  Roll table:      %zu alpha x %zu qbar\n",
                c.rollCmd.alpha_deg.size(), c.rollCmd.qbar.size());
    std::printf("  rawAuxAeroData:  %zu keys captured\n", c.rawAuxAeroData.size());
    return 0;
}
