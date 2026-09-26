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

#include "f4/convert/simple_convert_cli.hpp"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    return f4::convert::run_simple_convert(
        argc, argv,
        /*usage_args=*/"<FORMDAT.FIL> <output.json>",
        /*load=*/[](const std::string& in) {
            return f4::convert::loadFormFile(in);
        },
        /*write=*/[](const auto& r, const std::string& output_path) {
            return f4::data::writeFormationLibraryFile(r.data, output_path);
        },
        /*summarize=*/[](const auto& r) {
        std::printf("Formations: %zu\n", r.data.formations.size());
        for (const auto& f : r.data.formations) {
        std::printf("  %-10s formNum %d, %zu slots, 2-ship az %.1f deg / "
        "%.3f NM%s\n",
        f.name.c_str(), f.form_num, f.slots.size(),
        f.two_ship.rel_az_deg, f.two_ship.range_nm,
        f.two_ship_explicit ? " (explicit)" : " (from slot 0)");
        }
        });
}
