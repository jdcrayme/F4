// f4-renderer/tests/test_symbol_library.cpp
//
// Unit tests for the data-driven symbol library: data-model operations
// (find/add_or_replace/erase), JSON round-trip (parse + serialize),
// default library shape, and error handling for malformed input.
//
// No Raylib GPU context or ImGui needed — we test only the pure data
// model + JSON I/O. The render helpers (draw_library_symbol) are
// exercised at runtime by the Symbol Creator tool; a GPU-context smoke
// test would belong in the world-viewer's screenshot path.

#include <f4/renderer/symbol_library.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace f4::renderer;

// ===========================================================================
// Data model — SymbolLibrary find/add_or_replace/erase
// ===========================================================================

TEST(SymbolLibraryModel, EmptyLibraryHasZeroSize) {
    SymbolLibrary lib;
    EXPECT_TRUE(lib.empty());
    EXPECT_EQ(lib.size(), 0u);
    EXPECT_EQ(lib.find("anything"), nullptr);
}

TEST(SymbolLibraryModel, AddDefinitionAppends) {
    SymbolLibrary lib;
    SymbolDefinition s;
    s.key = "test_square";
    s.display_name = "Square";
    lib.add_or_replace(std::move(s));
    ASSERT_EQ(lib.size(), 1u);
    ASSERT_NE(lib.find("test_square"), nullptr);
    EXPECT_EQ(lib.find("test_square")->display_name, "Square");
}

TEST(SymbolLibraryModel, AddOrReplaceExistingKeyPreservesOrder) {
    SymbolLibrary lib;
    lib.add_or_replace({.key = "a"});
    lib.add_or_replace({.key = "b"});
    lib.add_or_replace({.key = "c"});

    // Replace "b" in place — order should stay [a, b, c].
    SymbolDefinition replacement;
    replacement.key = "b";
    replacement.display_name = "B (updated)";
    lib.add_or_replace(std::move(replacement));

    ASSERT_EQ(lib.size(), 3u);
    EXPECT_EQ(lib.symbols()[0].key, "a");
    EXPECT_EQ(lib.symbols()[1].key, "b");
    EXPECT_EQ(lib.symbols()[1].display_name, "B (updated)");
    EXPECT_EQ(lib.symbols()[2].key, "c");
}

TEST(SymbolLibraryModel, AddOrReplaceNewKeyAppends) {
    SymbolLibrary lib;
    lib.add_or_replace({.key = "a"});
    lib.add_or_replace({.key = "b"});
    lib.add_or_replace({.key = "c"});
    lib.add_or_replace({.key = "d"});

    ASSERT_EQ(lib.size(), 4u);
    EXPECT_EQ(lib.symbols()[3].key, "d");
}

TEST(SymbolLibraryModel, EraseExistingKeyReturnsTrue) {
    SymbolLibrary lib;
    lib.add_or_replace({.key = "a"});
    lib.add_or_replace({.key = "b"});
    lib.add_or_replace({.key = "c"});

    EXPECT_TRUE(lib.erase("b"));
    ASSERT_EQ(lib.size(), 2u);
    EXPECT_EQ(lib.symbols()[0].key, "a");
    EXPECT_EQ(lib.symbols()[1].key, "c");
    EXPECT_EQ(lib.find("b"), nullptr);
}

TEST(SymbolLibraryModel, EraseMissingKeyReturnsFalse) {
    SymbolLibrary lib;
    lib.add_or_replace({.key = "a"});
    EXPECT_FALSE(lib.erase("nonexistent"));
    EXPECT_EQ(lib.size(), 1u);
}

TEST(SymbolLibraryModel, MutableSymbolsAllowsInPlaceEdit) {
    SymbolLibrary lib;
    lib.add_or_replace({.key = "a", .display_name = "Old"});
    auto& syms = lib.mutable_symbols();
    syms[0].display_name = "New";
    EXPECT_EQ(lib.find("a")->display_name, "New");
}

// ===========================================================================
// JSON I/O — round-trip tests
// ===========================================================================

TEST(SymbolLibraryJson, EmptyLibraryRoundTrips) {
    SymbolLibrary lib;
    auto json = symbol_library_to_json(lib);
    auto lib2 = load_symbol_library_from_string(json);
    EXPECT_TRUE(lib2.empty());
    EXPECT_EQ(lib2.size(), 0u);
}

TEST(SymbolLibraryJson, SingleSymbolWithPolygonRoundTrips) {
    SymbolLibrary lib;
    SymbolDefinition s;
    s.key = "square";
    s.display_name = "Square";
    s.category = "example";
    s.description = "A filled square.";
    s.polygons.push_back({
        { {-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f} },
        true  // filled
    });
    lib.add_or_replace(std::move(s));

    auto json = symbol_library_to_json(lib);
    auto lib2 = load_symbol_library_from_string(json);

    ASSERT_EQ(lib2.size(), 1u);
    const auto* s2 = lib2.find("square");
    ASSERT_NE(s2, nullptr);
    EXPECT_EQ(s2->display_name, "Square");
    EXPECT_EQ(s2->category, "example");
    EXPECT_EQ(s2->description, "A filled square.");
    ASSERT_EQ(s2->polygons.size(), 1u);
    ASSERT_EQ(s2->polygons[0].points.size(), 4u);
    EXPECT_FLOAT_EQ(s2->polygons[0].points[0].x, -0.5f);
    EXPECT_FLOAT_EQ(s2->polygons[0].points[0].y, -0.5f);
    EXPECT_FLOAT_EQ(s2->polygons[0].points[2].x, 0.5f);
    EXPECT_FLOAT_EQ(s2->polygons[0].points[2].y, 0.5f);
    EXPECT_TRUE(s2->polygons[0].filled);
    EXPECT_TRUE(s2->polylines.empty());
}

TEST(SymbolLibraryJson, SingleSymbolWithPolylineRoundTrips) {
    SymbolLibrary lib;
    SymbolDefinition s;
    s.key = "line";
    s.display_name = "Line";
    s.polylines.push_back({
        { {-0.7f, 0.0f}, {0.7f, 0.0f} },
        2.5f,   // width
        false   // not closed
    });
    lib.add_or_replace(std::move(s));

    auto json = symbol_library_to_json(lib);
    auto lib2 = load_symbol_library_from_string(json);

    ASSERT_EQ(lib2.size(), 1u);
    const auto* s2 = lib2.find("line");
    ASSERT_NE(s2, nullptr);
    ASSERT_EQ(s2->polylines.size(), 1u);
    ASSERT_EQ(s2->polylines[0].points.size(), 2u);
    EXPECT_FLOAT_EQ(s2->polylines[0].width, 2.5f);
    EXPECT_FALSE(s2->polylines[0].closed);
}

TEST(SymbolLibraryJson, MultipleSymbolsPreserveOrder) {
    SymbolLibrary lib;
    lib.add_or_replace({.key = "alpha"});
    lib.add_or_replace({.key = "beta"});
    lib.add_or_replace({.key = "gamma"});

    auto json = symbol_library_to_json(lib);
    auto lib2 = load_symbol_library_from_string(json);

    ASSERT_EQ(lib2.size(), 3u);
    EXPECT_EQ(lib2.symbols()[0].key, "alpha");
    EXPECT_EQ(lib2.symbols()[1].key, "beta");
    EXPECT_EQ(lib2.symbols()[2].key, "gamma");
}

TEST(SymbolLibraryJson, UnknownKeysAreSkipped) {
    // Forward-compat: a future schema with extra fields should parse.
    const std::string json = R"({
      "version": 1,
      "future_field": "ignored",
      "symbols": [
        {
          "key": "test",
          "display_name": "Test",
          "future_per_symbol_field": 42,
          "polylines": [],
          "polygons": []
        }
      ]
    })";
    auto lib = load_symbol_library_from_string(json);
    ASSERT_EQ(lib.size(), 1u);
    EXPECT_EQ(lib.find("test")->display_name, "Test");
}

TEST(SymbolLibraryJson, MissingOptionalFieldsDefaultToEmpty) {
    const std::string json = R"({
      "version": 1,
      "symbols": [
        { "key": "minimal" }
      ]
    })";
    auto lib = load_symbol_library_from_string(json);
    ASSERT_EQ(lib.size(), 1u);
    const auto* s = lib.find("minimal");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->display_name, "");
    EXPECT_EQ(s->category, "");
    EXPECT_EQ(s->description, "");
    EXPECT_TRUE(s->polylines.empty());
    EXPECT_TRUE(s->polygons.empty());
}

TEST(SymbolLibraryJson, MissingWidthDefaultsToOne) {
    const std::string json = R"({
      "symbols": [
        {
          "key": "default_width",
          "polylines": [
            { "points": [ {"x": -0.5, "y": 0.0}, {"x": 0.5, "y": 0.0} ] }
          ]
        }
      ]
    })";
    auto lib = load_symbol_library_from_string(json);
    const auto* s = lib.find("default_width");
    ASSERT_NE(s, nullptr);
    ASSERT_EQ(s->polylines.size(), 1u);
    EXPECT_FLOAT_EQ(s->polylines[0].width, 1.0f);
    EXPECT_FALSE(s->polylines[0].closed);
}

TEST(SymbolLibraryJson, MissingFilledDefaultsToTrue) {
    const std::string json = R"({
      "symbols": [
        {
          "key": "default_filled",
          "polygons": [
            { "points": [ {"x": -0.5, "y": -0.5}, {"x": 0.5, "y": -0.5}, {"x": 0.0, "y": 0.5} ] }
          ]
        }
      ]
    })";
    auto lib = load_symbol_library_from_string(json);
    const auto* s = lib.find("default_filled");
    ASSERT_NE(s, nullptr);
    ASSERT_EQ(s->polygons.size(), 1u);
    EXPECT_TRUE(s->polygons[0].filled);
}

TEST(SymbolLibraryJson, NegativeCoordinatesRoundTrip) {
    SymbolLibrary lib;
    SymbolDefinition s;
    s.key = "neg";
    s.polylines.push_back({
        { {-1.0f, -1.0f}, {1.0f, 1.0f} },
        1.0f,
        false
    });
    lib.add_or_replace(std::move(s));

    auto json = symbol_library_to_json(lib);
    auto lib2 = load_symbol_library_from_string(json);

    const auto* s2 = lib2.find("neg");
    ASSERT_NE(s2, nullptr);
    ASSERT_EQ(s2->polylines[0].points.size(), 2u);
    EXPECT_FLOAT_EQ(s2->polylines[0].points[0].x, -1.0f);
    EXPECT_FLOAT_EQ(s2->polylines[0].points[0].y, -1.0f);
    EXPECT_FLOAT_EQ(s2->polylines[0].points[1].x, 1.0f);
    EXPECT_FLOAT_EQ(s2->polylines[0].points[1].y, 1.0f);
}

TEST(SymbolLibraryJson, FractionalWidthRoundTrips) {
    SymbolLibrary lib;
    SymbolDefinition s;
    s.key = "frac";
    s.polylines.push_back({
        { {-0.5f, 0.0f}, {0.5f, 0.0f} },
        0.125f,
        false
    });
    lib.add_or_replace(std::move(s));

    auto json = symbol_library_to_json(lib);
    auto lib2 = load_symbol_library_from_string(json);
    EXPECT_FLOAT_EQ(lib2.find("frac")->polylines[0].width, 0.125f);
}

TEST(SymbolLibraryJson, ClosedPolylineFlagRoundTrips) {
    SymbolLibrary lib;
    SymbolDefinition s;
    s.key = "closed";
    s.polylines.push_back({
        { {0.0f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f} },
        1.0f,
        true  // closed
    });
    lib.add_or_replace(std::move(s));

    auto json = symbol_library_to_json(lib);
    auto lib2 = load_symbol_library_from_string(json);
    EXPECT_TRUE(lib2.find("closed")->polylines[0].closed);
}

TEST(SymbolLibraryJson, RoundTripsFullDefaultLibrary) {
    auto lib = make_default_symbol_library();
    ASSERT_EQ(lib.size(), 3u);
    auto json = symbol_library_to_json(lib);
    auto lib2 = load_symbol_library_from_string(json);
    ASSERT_EQ(lib2.size(), 3u);

    // Check the three example symbols survived intact.
    ASSERT_NE(lib2.find("example_square"), nullptr);
    ASSERT_NE(lib2.find("example_triangle"), nullptr);
    ASSERT_NE(lib2.find("example_diamond"), nullptr);

    // example_square: 1 polygon (filled) + 1 polyline.
    const auto* sq = lib2.find("example_square");
    EXPECT_EQ(sq->polygons.size(), 1u);
    EXPECT_TRUE(sq->polygons[0].filled);
    EXPECT_EQ(sq->polygons[0].points.size(), 4u);
    EXPECT_EQ(sq->polylines.size(), 1u);
    EXPECT_EQ(sq->polylines[0].points.size(), 2u);

    // example_triangle: 1 polygon (outline only).
    const auto* tri = lib2.find("example_triangle");
    EXPECT_EQ(tri->polygons.size(), 1u);
    EXPECT_FALSE(tri->polygons[0].filled);
    EXPECT_EQ(tri->polygons[0].points.size(), 3u);

    // example_diamond: 1 polygon (filled) + 1 polyline.
    const auto* dia = lib2.find("example_diamond");
    EXPECT_EQ(dia->polygons.size(), 1u);
    EXPECT_TRUE(dia->polygons[0].filled);
    EXPECT_EQ(dia->polygons[0].points.size(), 4u);
    EXPECT_EQ(dia->polylines.size(), 1u);
}

// ===========================================================================
// File I/O — load/save to actual disk paths
// ===========================================================================

TEST(SymbolLibraryJson, SaveAndLoadFileRoundTrip) {
    namespace fs = std::filesystem;
    const auto tmp_dir = fs::temp_directory_path() / "f4_symbol_library_test";
    fs::create_directories(tmp_dir);
    const auto tmp_file = tmp_dir / "test_lib.json";

    // Clean up any stale file from a previous run.
    if (fs::exists(tmp_file)) {
        fs::remove(tmp_file);
    }

    SymbolLibrary lib;
    lib.add_or_replace({.key = "file_test", .display_name = "File Test"});

    ASSERT_NO_THROW(save_symbol_library(lib, tmp_file));
    ASSERT_TRUE(fs::exists(tmp_file));

    auto lib2 = load_symbol_library(tmp_file);
    ASSERT_EQ(lib2.size(), 1u);
    EXPECT_EQ(lib2.find("file_test")->display_name, "File Test");

    // Cleanup.
    fs::remove(tmp_file);
    fs::remove(tmp_dir);
}

TEST(SymbolLibraryJson, LoadNonexistentFileThrows) {
    namespace fs = std::filesystem;
    const auto tmp_dir = fs::temp_directory_path() / "f4_symbol_library_nonexistent";
    const auto nope = tmp_dir / "does_not_exist.json";
    EXPECT_THROW(load_symbol_library(nope), std::runtime_error);
}

// ===========================================================================
// Error handling — malformed JSON should throw with a position-annotated msg
// ===========================================================================

TEST(SymbolLibraryJson, MalformedJsonThrows) {
    const std::string malformed = R"({ "symbols": [ { "key":  }] })";
    // "key": <missing value> — the reader should throw.
    EXPECT_THROW(load_symbol_library_from_string(malformed), std::runtime_error);
}

TEST(SymbolLibraryJson, EmptyStringThrows) {
    EXPECT_THROW(load_symbol_library_from_string(""), std::runtime_error);
}

TEST(SymbolLibraryJson, EmptyObjectYieldsEmptyLibrary) {
    auto lib = load_symbol_library_from_string("{}");
    EXPECT_TRUE(lib.empty());
}

TEST(SymbolLibraryJson, EmptySymbolsArrayYieldsEmptyLibrary) {
    const std::string json = R"({ "version": 1, "symbols": [] })";
    auto lib = load_symbol_library_from_string(json);
    EXPECT_TRUE(lib.empty());
}

// ===========================================================================
// Default library — sanity-check the seed content the editor ships with.
// ===========================================================================

TEST(DefaultLibrary, HasThreeExamples) {
    auto lib = make_default_symbol_library();
    EXPECT_EQ(lib.size(), 3u);
}

TEST(DefaultLibrary, AllKeysAreExamplePrefixed) {
    auto lib = make_default_symbol_library();
    for (const auto& s : lib.symbols()) {
        EXPECT_EQ(s.key.substr(0, 8), "example_")
            << "key '" << s.key << "' should start with 'example_'";
    }
}

TEST(DefaultLibrary, AllExamplesHaveNonEmptyDisplayNames) {
    auto lib = make_default_symbol_library();
    for (const auto& s : lib.symbols()) {
        EXPECT_FALSE(s.display_name.empty())
            << "key '" << s.key << "' has an empty display_name";
    }
}

TEST(DefaultLibrary, AllExamplesHaveAtLeastOnePrimitive) {
    auto lib = make_default_symbol_library();
    for (const auto& s : lib.symbols()) {
        const std::size_t total = s.polylines.size() + s.polygons.size();
        EXPECT_GE(total, 1u)
            << "key '" << s.key << "' has no polylines or polygons";
    }
}
