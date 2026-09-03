// f4-convert/cli/sens2json.cpp
//
// CLI tool: convert a FreeFalcon SENSDATA sensor list (.LST + the files
// it references, resolved in the same directory) to f4 JSON format.
//
// Usage:
//   sens2json <irst|rwr|visual> <list.LST> <output.json>
//
// Exit codes:
//   0  success
//   1  bad arguments
//   2  parse failure

#include "f4/convert/sensor_parser.hpp"
#include "f4/data/sensor_data.hpp"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr,
                     "Usage: %s <irst|rwr|visual> <list.LST> <output.json>\n",
                     argv[0]);
        return 1;
    }

    const std::string kind = argv[1];
    const std::string inputPath = argv[2];
    const std::string outputPath = argv[3];
    std::printf("Converting %s (%s) -> %s\n", inputPath.c_str(), kind.c_str(),
                outputPath.c_str());

    if (kind == "irst") {
        auto result = f4::convert::loadIrstListFile(inputPath);
        if (!result.ok) {
            for (auto const& e : result.errors)
                std::fprintf(stderr, "  %s\n", e.c_str());
            return 2;
        }
        for (auto const& w : result.warnings)
            std::printf("WARNING: %s\n", w.c_str());
        if (!f4::data::writeIrstSensorDataFile(result.data, outputPath)) {
            std::fprintf(stderr, "ERROR: failed to write %s\n",
                         outputPath.c_str());
            return 3;
        }
        std::printf("IRST sensors: %zu\n", result.data.sensors.size());
        return 0;
    }
    if (kind == "rwr") {
        auto result = f4::convert::loadRwrListFile(inputPath);
        if (!result.ok) {
            for (auto const& e : result.errors)
                std::fprintf(stderr, "  %s\n", e.c_str());
            return 2;
        }
        for (auto const& w : result.warnings)
            std::printf("WARNING: %s\n", w.c_str());
        if (!f4::data::writeRwrSensorDataFile(result.data, outputPath)) {
            std::fprintf(stderr, "ERROR: failed to write %s\n",
                         outputPath.c_str());
            return 3;
        }
        std::printf("RWR receivers: %zu\n", result.data.sensors.size());
        return 0;
    }
    if (kind == "visual") {
        auto result = f4::convert::loadVisualListFile(inputPath);
        if (!result.ok) {
            for (auto const& e : result.errors)
                std::fprintf(stderr, "  %s\n", e.c_str());
            return 2;
        }
        for (auto const& w : result.warnings)
            std::printf("WARNING: %s\n", w.c_str());
        if (!f4::data::writeVisualSensorDataFile(result.data, outputPath)) {
            std::fprintf(stderr, "ERROR: failed to write %s\n",
                         outputPath.c_str());
            return 3;
        }
        std::printf("Visual sensors: %zu\n", result.data.sensors.size());
        return 0;
    }

    std::fprintf(stderr,
                 "ERROR: unknown kind '%s' (want irst, rwr or visual)\n",
                 kind.c_str());
    return 1;
}
