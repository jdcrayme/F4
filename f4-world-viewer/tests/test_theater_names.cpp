// f4-world-viewer/tests/test_theater_names.cpp
//
// Unit tests for the theater name table loader (theater_names.hpp/cpp —
// deliberately ImGui-free). The table is scripts/export_names.py's
// f4.theater.names/1 document; the lookup bounds-check is the save-vs-
// install safety net (a save made under a larger name table must fall
// back to raw-id rendering, never read OOB).

#include "../src/theater_names.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

using namespace f4::viewer;

namespace {

const char* kDoc = R"({
 "format": "f4.theater.names/1",
 "theater": "korea",
 "count": 4,
 "names": ["Nowhere", "New", "East Incheon", "Posong-ni"]
})";

} // namespace

TEST(TheaterNames, ParsesDocument) {
    const auto names = load_theater_names(kDoc);
    ASSERT_EQ(names.size(), 4u);
    EXPECT_EQ(names[0], "Nowhere");
    EXPECT_EQ(names[1], "New");
    EXPECT_EQ(names[3], "Posong-ni");
}

TEST(TheaterNames, RejectsMalformed) {
    EXPECT_THROW(load_theater_names("{}"), std::runtime_error);
    EXPECT_THROW(load_theater_names(""), std::runtime_error);
    EXPECT_THROW(load_theater_names(
                     R"({"format":"something/else","names":[]})"),
                 std::runtime_error);
    EXPECT_THROW(load_theater_names(R"({"theater":"korea","names":[]})"),
                 std::runtime_error);   // no format tag
    // A truncated array must throw rather than return a partial table.
    EXPECT_THROW(load_theater_names(
                     R"({"format":"f4.theater.names/1","names":["a",)"),
                 std::runtime_error);
}

TEST(TheaterNames, LookupBoundsChecked) {
    const auto names = load_theater_names(kDoc);
    EXPECT_EQ(theater_name_for_id(names, 0), "Nowhere");
    EXPECT_EQ(theater_name_for_id(names, 3), "Posong-ni");
    // Past the table: the empty string (the caller renders the raw id).
    EXPECT_EQ(theater_name_for_id(names, 4), "");
    EXPECT_EQ(theater_name_for_id(names, 3824), "");
    EXPECT_EQ(theater_name_for_id(names, -1), "");
    // An empty table (asset absent) never resolves.
    EXPECT_EQ(theater_name_for_id({}, 0), "");
}
