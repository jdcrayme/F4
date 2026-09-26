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

#include "f4/convert/simple_convert_cli.hpp"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    return f4::convert::run_simple_convert(
        argc, argv,
        /*usage_args=*/"<Vehicle.lst> <output.json>",
        /*load=*/[](const std::string& in) {
            return f4::convert::loadVehicleLstFile(in);
        },
        /*write=*/[](const auto& r, const std::string& output_path) {
            return f4::data::writeVehicleDefinitionLibraryFile(r.library, output_path);
        },
        /*summarize=*/[](const auto& r) {
        std::printf("Vehicle class rows:  %zu\n", r.library.entries.size());
        for (int t = 0; t <= 4; ++t) {
        std::printf("  %-10s %zu\n", f4::data::kMoverTypeNames[t],
        r.library.count_of_type(
        static_cast<f4::data::MoverType>(t)));
        }
        });
}
