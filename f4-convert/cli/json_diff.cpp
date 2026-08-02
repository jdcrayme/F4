// f4-convert/cli/json_diff.cpp
//
// CLI tool: diff two JSON aircraft files field-by-field.
//
// Loads both files into AircraftConfig structs and compares every field. The
// comparison is structural (not textual), so whitespace/key-order differences
// in the JSON source are ignored. Output is a unified-diff-style report
// listing every field that differs, with the old and new values.
//
// Exit codes:
//   0  files are equivalent (no differences)
//   1  files differ
//   2  could not load one or both files
//
// Usage:
//   json_diff <old.json> <new.json>
//   json_diff <old.json> <new.json> --summary     # only print counts, not details
//   json_diff <old.json> <new.json> --threshold 1e-9   # numeric tolerance (default 1e-12)

#include "f4/convert/json_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Options {
    bool summary_only = false;
    double tolerance = 1e-12;
};

Options parseArgs(int argc, char** argv, std::string& pathA, std::string& pathB) {
    Options opts;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--summary") {
            opts.summary_only = true;
        } else if (arg == "--threshold" && i + 1 < argc) {
            opts.tolerance = std::stod(argv[++i]);
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.size() >= 2) {
        pathA = positional[0];
        pathB = positional[1];
    }
    return opts;
}

} // namespace

int main(int argc, char** argv) {
    std::string pathA, pathB;
    Options opts = parseArgs(argc, argv, pathA, pathB);
    if (pathA.empty() || pathB.empty()) {
        std::fprintf(stderr, "Usage: %s <old.json> <new.json> [--summary] [--threshold 1e-9]\n", argv[0]);
        return 2;
    }

    f4::data::AircraftConfig cfgA, cfgB;
    auto resA = f4::convert::readJsonFile(pathA, cfgA);
    auto resB = f4::convert::readJsonFile(pathB, cfgB);
    if (!resA.ok) {
        std::fprintf(stderr, "ERROR loading %s:\n", pathA.c_str());
        for (auto const& e : resA.errors) std::fprintf(stderr, "  %s\n", e.c_str());
        return 2;
    }
    if (!resB.ok) {
        std::fprintf(stderr, "ERROR loading %s:\n", pathB.c_str());
        for (auto const& e : resB.errors) std::fprintf(stderr, "  %s\n", e.c_str());
        return 2;
    }

    auto diffs = f4::convert::diffConfigs(cfgA, cfgB, opts.tolerance);

    if (diffs.empty()) {
        std::printf("Files are equivalent (0 differences within tolerance %g)\n", opts.tolerance);
        return 0;
    }

    std::printf("Files differ (%zu field-level differences):\n", diffs.size());
    if (!opts.summary_only) {
        for (auto const& d : diffs) std::printf("%s\n", d.c_str());
    }
    return 1;
}
