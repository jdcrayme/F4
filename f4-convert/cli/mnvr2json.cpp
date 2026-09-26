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

#include "f4/convert/simple_convert_cli.hpp"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    return f4::convert::run_simple_convert(
        argc, argv,
        /*usage_args=*/"<mnvrdata.dat> <output.json>",
        /*load=*/[](const std::string& in) {
            return f4::convert::loadMnvFile(in);
        },
        /*write=*/[](const auto& r, const std::string& output_path) {
            return f4::data::writeManeuverDataFile(r.data, output_path);
        },
        /*summarize=*/[](const auto& r) {
        std::printf("Maneuver classes:      9\n");
        std::printf("Populated 9x9 cells:   %zu\n",
        r.data.populatedCells());
        for (std::size_t i = 0; i < f4::data::kNumMnvrClasses; ++i) {
        std::printf("  %-8s flags 0x%X\n", f4::data::kMnvrClassNames[i],
        r.data.classFlags[i]);
        }
        });
}
