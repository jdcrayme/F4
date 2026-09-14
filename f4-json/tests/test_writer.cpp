// test_writer.cpp — f4::json::Writer

#include <gtest/gtest.h>
#include <f4/json/f4_json.hpp>
#include <filesystem>
#include <string>

using f4::json::Reader;
using f4::json::Writer;

TEST(JsonWriter, EmptyByDefault) {
    Writer w;
    EXPECT_EQ(w.size(), 0u);
    EXPECT_TRUE(w.str().empty());
}

TEST(JsonWriter, RawString) {
    Writer w;
    w.raw("{\n  \"hello\": 1\n}\n");
    EXPECT_EQ(w.str(), "{\n  \"hello\": 1\n}\n");
}

TEST(JsonWriter, StringLiteralEscapesProperly) {
    Writer w;
    w.string("a\\b\"c\n");
    EXPECT_EQ(w.str(), "\"a\\\\b\\\"c\\n\"");
}

TEST(JsonWriter, StringWithControlChars) {
    Writer w;
    w.string(std::string("\x01\x02\x1f", 3));
    EXPECT_EQ(w.str(), "\"\\u0001\\u0002\\u001f\"");
}

TEST(JsonWriter, StringWithHighBytesPassesThrough) {
    // Non-ASCII bytes (UTF-8 continuation) pass through verbatim.
    Writer w;
    w.string("\xc3\xa9");  // é in UTF-8
    EXPECT_EQ(w.str(), "\"\xc3\xa9\"");
}

TEST(JsonWriter, NumberLong) {
    Writer w;
    w.number(12345L);
    EXPECT_EQ(w.str(), "12345");
}

TEST(JsonWriter, NumberNegative) {
    Writer w;
    w.number(-42L);
    EXPECT_EQ(w.str(), "-42");
}

TEST(JsonWriter, NumberUnsigned) {
    Writer w;
    w.number(4294967295UL);
    EXPECT_EQ(w.str(), "4294967295");
}

TEST(JsonWriter, NumberDoubleRoundTrips) {
    Writer w;
    w.number(3.14159265358979);
    // Re-parse and verify round-trip preserves precision.
    Reader r(w.str());
    double v = r.read_number();
    EXPECT_NEAR(v, 3.14159265358979, 1e-12);
}

TEST(JsonWriter, NumberDoubleInteger) {
    Writer w;
    w.number(100.0);
    Reader r(w.str());
    double v = r.read_number();
    EXPECT_NEAR(v, 100.0, 1e-9);
}

TEST(JsonWriter, StringKeyEmitsQuoted) {
    Writer w;
    w.string_key("name", "Korea");
    EXPECT_EQ(w.str(), "\"name\":\"Korea\"");
}

TEST(JsonWriter, NumberKeyLong) {
    Writer w;
    w.number_key("width", 128L);
    EXPECT_EQ(w.str(), "\"width\":128");
}

TEST(JsonWriter, NumberKeyUnsigned) {
    Writer w;
    w.number_key("id", 0xdeadbeefUL);
    EXPECT_EQ(w.str(), "\"id\":3735928559");
}

TEST(JsonWriter, NumberKeyDouble) {
    Writer w;
    w.number_key("pi", 3.14);
    Reader r(w.str());
    std::string key = r.read_string();  // handles the opening/closing quotes
    EXPECT_EQ(key, "pi");
    r.expect(':');
    double v = r.read_number();
    EXPECT_NEAR(v, 3.14, 1e-9);
}

TEST(JsonWriter, ComposeSmallObject) {
    Writer w;
    w.raw("{\n");
    w.raw("  "); w.string_key("theater", "Korea"); w.raw(",\n");
    w.raw("  "); w.number_key("width", 128L);      w.raw(",\n");
    w.raw("  "); w.number_key("height", 128L);     w.raw("\n");
    w.raw("}\n");

    // Verify by re-parsing.
    Reader r(w.str());
    r.skip_ws();
    r.expect('{');
    std::string theater;
    long long width = 0, height = 0;
    while (!r.consume('}')) {
        std::string k = r.read_string();
        r.expect(':');
        if (k == "theater") theater = r.read_string();
        else if (k == "width")  width  = r.read_int();
        else if (k == "height") height = r.read_int();
        else r.skip_value();
        (void)r.consume(',');
    }
    EXPECT_EQ(theater, "Korea");
    EXPECT_EQ(width, 128);
    EXPECT_EQ(height, 128);
}

// ── escape_string — hand-built documents interpolating external strings ──
//
// The canonical trap: a raw native Windows path pasted into a hand-built
// JSON document. "\f" is a form feed, so "...\generated_fixtures\f16.json"
// silently parsed (under the lenient reader) as "...\generated_fixtures"
// + FF + "16.json" — miles from where the document was built.

TEST(JsonWriter, EscapeStringPassesPlainTextThrough) {
    EXPECT_EQ(f4::json::escape_string("Data/Aircraft/f16.json"),
              "Data/Aircraft/f16.json");
    EXPECT_TRUE(f4::json::escape_string("").empty());
}

TEST(JsonWriter, EscapeStringEscapesBackslashesAndQuotes) {
    // Input carries single backslashes (C++ source doubles them); the
    // escaped output carries doubled backslashes (source quadruples).
    EXPECT_EQ(f4::json::escape_string("E:\\Code\\F4\\f16.json"),
              "E:\\\\Code\\\\F4\\\\f16.json");
    EXPECT_EQ(f4::json::escape_string("say \"hi\""), "say \\\"hi\\\"");
}

TEST(JsonWriter, EscapeStringEscapesControlCharacters) {
    EXPECT_EQ(f4::json::escape_string(std::string("a\nb\tc")),
              "a\\nb\\tc");
    // A literal form feed (what the lenient reader USED to turn "\f" into).
    EXPECT_EQ(f4::json::escape_string(std::string("a\fb")),
              "a\\fb");
    // Control characters below 0x20 without a short escape go as \u00XX.
    EXPECT_EQ(f4::json::escape_string(std::string("a\x01" "b")),
              "a\\u0001b");
}

TEST(JsonWriter, EscapedPathRoundTripsThroughReader) {
    const std::string native =
        (std::filesystem::path("E:/Code/F4/Build") / "generated_fixtures" /
         "f16.json").string();
    Writer w;
    w.string(native);
    Reader r(w.str());
    EXPECT_EQ(r.read_string(), native);
}
