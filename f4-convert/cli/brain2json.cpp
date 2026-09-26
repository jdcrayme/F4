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

#include "f4/convert/simple_convert_cli.hpp"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    return f4::convert::run_simple_convert(
        argc, argv,
        /*usage_args=*/"<input.brn> <output.json>",
        /*load=*/[](const std::string& in) {
            return f4::convert::loadBrainFile(in);
        },
        /*write=*/[](const auto& r, const std::string& output_path) {
            return f4::data::writeBrainDataFile(r.data, output_path);
        },
        /*summarize=*/[](const auto& r) {
        std::printf("Archetypes: %zu\n", r.data.archetypes.size());
        for (const auto& a : r.data.archetypes) {
        std::printf("  %-12s %zu mode rows\n", a.name.c_str(),
        a.modes.size());
        }
        if (r.max_gs != 0.0) {
        std::printf("Max Gs trailer: %.1f\n", r.max_gs);
        }
        });
}
