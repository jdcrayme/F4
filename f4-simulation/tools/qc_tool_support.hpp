// f4-simulation/tools/qc_tool_support.hpp
//
// The CLI scaffolding shared verbatim by the three combat QC tools
// (bvr_intercept_qc, wvr_merge_qc, ground_strike_qc): the Args option
// struct, the parse loop, and the JSON string helpers. The per-harness
// summary/artifact emission stays in each tool - only the plumbing is
// shared, and the exit-code contract keeps living in one parse.
//
// Each tool defines its own usage() (tool-specific option text) and
// includes this header; parse_args resolves it via the declaration below.

#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <f4/json/writer.hpp>

// Defined per tool — the option summary text differs per harness.
[[noreturn]] void usage(const char* prog);

struct Args {
    std::filesystem::path scenario_json;
    std::filesystem::path out_dir;
    std::int64_t horizon_sec = 300;   // 5 min — the M4 acceptance horizon
    double sample_sec = 30.0;         // diary + check cadence
    int runs = 2;                     // 2 = the determinism proof; 1 = skip
    double max_wall_sec = 0.0;        // 0 = watchdog off
    bool quiet = false;
};


inline Args parse_args(int argc, char** argv) {
    Args a;
    if (argc < 2) usage(argv[0]);

    // --help can appear anywhere; handle it before the positional.
    for (int i = 1; i < argc; ++i) {
        const std::string tok = argv[i];
        if (tok == "--help" || tok == "-h") usage(argv[0]);
    }

    a.scenario_json = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string k = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) usage(argv[0]);
            return argv[++i];
        };
        if (k == "--horizon-sec") {
            a.horizon_sec = std::strtoll(next(), nullptr, 10);
        } else if (k == "--sample-sec") {
            a.sample_sec = std::atof(next());
        } else if (k == "--runs") {
            a.runs = std::max(1, std::atoi(next()));
        } else if (k == "--out-dir") {
            a.out_dir = next();
        } else if (k == "--max-wall") {
            a.max_wall_sec = std::atof(next());
        } else if (k == "--quiet") {
            a.quiet = true;
        } else {
            std::fprintf(stderr, "unknown option '%s'\n", k.c_str());
            usage(argv[0]);
        }
    }
    if (a.out_dir.empty()) a.out_dir = a.scenario_json.parent_path();
    return a;
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

void write_string(f4::json::Writer& w, const std::string& s) {
    w.put('"');
    w.put(json_escape(s));
    w.put('"');
}
