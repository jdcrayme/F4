// f4-convert/include/f4/convert/simple_convert_cli.hpp
//
// run_simple_convert -- the two-argument converter CLI skeleton shared by
// the f4-convert tools. Each tool contributes three steps: its parser
// call, its writer call, and its summary printer. The skeleton owns the
// usage check, the "Converting" banner, and the house error/warning
// reporting, so the exit-code contract lives in exactly one place.
//
// Exit codes: 0 success, 1 usage, 2 parse failure, 3 write failure.
// (Tools with richer contracts -- dat2json AFM-skip 4, sens2json kind
// dispatch -- keep their own mains.)

#pragma once

#include <cstdio>
#include <string>
#include <string_view>

namespace f4::convert {

template <class Load, class Write, class Summarize>
int run_simple_convert(int argc, char** argv, std::string_view usage_args,
                       Load&& load, Write&& write, Summarize&& summarize) {
    if (argc != 3) {
        std::fprintf(stderr, "Usage: %s %.*s\n", argv[0],
                     static_cast<int>(usage_args.size()), usage_args.data());
        return 1;
    }

    const std::string input_path  = argv[1];
    const std::string output_path = argv[2];
    std::printf("Converting %s -> %s\n", input_path.c_str(),
                output_path.c_str());

    auto result = load(input_path);
    if (!result.ok) {
        std::fprintf(stderr, "ERROR: failed to parse %s\n",
                     input_path.c_str());
        for (auto const& e : result.errors)
            std::fprintf(stderr, "  %s\n", e.c_str());
        return 2;
    }
    for (auto const& w : result.warnings)
        std::printf("WARNING: %s\n", w.c_str());

    if (!write(result, output_path)) {
        std::fprintf(stderr, "ERROR: failed to write %s\n",
                     output_path.c_str());
        return 3;
    }

    summarize(result);
    return 0;
}

} // namespace f4::convert
