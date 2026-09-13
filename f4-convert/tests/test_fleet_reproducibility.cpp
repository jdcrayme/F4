// f4-convert/tests/test_fleet_reproducibility.cpp
//
// The committed fleet (Data/Aircraft/*.json) is exactly reproducible from
// the committed fixtures: for every aircraft whose .dat is in
// f4-convert/tests/fixtures/, parse -> writeConfig must produce the
// committed file BYTE FOR BYTE. The pipeline is the source of truth; the
// committed JSONs are its pinned output — a converter change that would
// silently alter the fleet fails here, loudly.
//
// kc10.json has no committed .dat fixture (see CHANGES.md, Task 72) and is
// covered by test_fleet_auxaero's reconstructed-record assertions instead.
//
// C++20. GoogleTest.

#include <f4/convert/dat_parser.hpp>
#include <f4/data/config_loader.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

constexpr const char* kFixturesDir = F4_CONVERT_TEST_FIXTURES_DIR;
constexpr const char* kFleetDir = F4_SOURCE_DIR "/Data/Aircraft";

std::vector<std::filesystem::path> fixtureDats() {
    std::vector<std::filesystem::path> out;
    for (const auto& e : std::filesystem::directory_iterator(kFixturesDir)) {
        if (e.path().extension() == ".dat" &&
            e.path().stem().string() != "test_synthetic") {
            out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

TEST(FleetReproducibility, DatFixtureRegeneratesCommittedJsonByteForByte) {
    ASSERT_TRUE(std::filesystem::is_directory(kFleetDir))
        << "committed Data/Aircraft not found";

    int checked = 0;
    for (const auto& dat : fixtureDats()) {
        const std::string stem = dat.stem().string();
        const std::filesystem::path committed =
            std::filesystem::path(kFleetDir) / (stem + ".json");
        ASSERT_TRUE(std::filesystem::exists(committed))
            << "fixture without a committed fleet JSON: " << stem;

        auto result = f4::convert::loadFile(dat.string());
        ASSERT_TRUE(result.ok) << stem << ": " << result.errors[0];

        const std::string regenerated = f4::data::writeConfig(result.config);
        const std::string expected = slurp(committed);
        EXPECT_EQ(regenerated, expected)
            << stem << ": the committed fleet JSON is not what dat2json "
                         "produces from the committed fixture";

        ++checked;
    }
    EXPECT_EQ(checked, 24) << "expected the full 24-fixture fleet";
}
