// test_roundtrip.cpp — emit a JSON doc with Writer, re-parse with Reader,
// verify field-by-field equality. Catches the most common JSON bug class
// (escaping mismatches between writer and reader) at the integration level.

#include <gtest/gtest.h>
#include <f4/json/f4_json.hpp>
#include <string>
#include <vector>

using f4::json::Reader;
using f4::json::Writer;

namespace {

// Sample document shape — mirrors the terrain.json schema (string fields,
// numeric fields, a flat array of integers).
std::string emit_sample(const std::string& theater, long width, long height,
                        const std::vector<int>& tiles) {
    Writer w;
    w.raw("{\n");
    w.raw("  \"theater\": ");  w.string(theater);    w.raw(",\n");
    w.raw("  \"width\": ");    w.number(width);      w.raw(",\n");
    w.raw("  \"height\": ");   w.number(height);     w.raw(",\n");
    w.raw("  \"tile_types\": [");
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        if (i) w.raw(",");
        w.number(static_cast<long>(tiles[i]));
    }
    w.raw("]\n");
    w.raw("}\n");
    return w.str();
}

} // namespace

TEST(JsonRoundtrip, RoundtripsPreserveAllFields) {
    auto json = emit_sample("Korea", 4, 2, {1, 2, 3, 4, 5, 6, 7, 8});

    Reader r(json);
    r.skip_ws();
    r.expect('{');

    std::string theater;
    long width = 0, height = 0;
    std::vector<int> tiles;
    while (!r.consume('}')) {
        std::string k = r.read_string();
        r.expect(':');
        if (k == "theater") theater = r.read_string();
        else if (k == "width")  width  = r.read_int();
        else if (k == "height") height = r.read_int();
        else if (k == "tile_types") {
            r.skip_ws();
            r.expect('[');
            if (!r.peek(']')) for (;;) {
                tiles.push_back(static_cast<int>(r.read_int()));
                if (r.consume(']')) break;
                r.expect(',');
            }
        } else {
            r.skip_value();
        }
        (void)r.consume(',');
    }

    EXPECT_EQ(theater, "Korea");
    EXPECT_EQ(width, 4);
    EXPECT_EQ(height, 2);
    ASSERT_EQ(tiles.size(), 8u);
    for (int i = 0; i < 8; ++i) EXPECT_EQ(tiles[i], i + 1);
}

TEST(JsonRoundtrip, StringsThatNeedEscapingSurviveRoundtrip) {
    Writer w;
    w.raw("{\"path\": ");
    w.string("C:\\Falcon4\\campaign\\save1.cam");
    w.raw(", \"note\": ");
    w.string("line1\nline2\ttabbed");
    w.raw("}");

    Reader r(w.str());
    r.skip_ws();
    r.expect('{');
    std::string path, note;
    while (!r.consume('}')) {
        std::string k = r.read_string();
        r.expect(':');
        if (k == "path") path = r.read_string();
        else if (k == "note") note = r.read_string();
        else r.skip_value();
        (void)r.consume(',');
    }
    EXPECT_EQ(path, "C:\\Falcon4\\campaign\\save1.cam");
    EXPECT_EQ(note, "line1\nline2\ttabbed");
}

TEST(JsonRoundtrip, EmptyArrayRoundtrips) {
    Writer w;
    w.raw("{\"items\": []}");

    Reader r(w.str());
    r.skip_ws();
    r.expect('{');
    int count = -1;
    while (!r.consume('}')) {
        std::string k = r.read_string();
        r.expect(':');
        if (k == "items") {
            r.skip_ws();
            r.expect('[');
            count = 0;
            if (!r.consume(']')) {
                for (;;) {
                    r.skip_value();
                    ++count;
                    if (r.consume(']')) break;
                    r.expect(',');
                }
            }
        } else {
            r.skip_value();
        }
        (void)r.consume(',');
    }
    EXPECT_EQ(count, 0);
}

TEST(JsonRoundtrip, NestedObjectRoundtrips) {
    Writer w;
    w.raw("{");
    w.raw("\"a\": {\"b\": {\"c\": 42}}");
    w.raw("}");

    Reader r(w.str());
    r.skip_ws();
    r.expect('{');
    long c = 0;
    while (!r.consume('}')) {
        std::string k = r.read_string();
        r.expect(':');
        if (k == "a") {
            r.skip_ws();
            r.expect('{');
            while (!r.consume('}')) {
                std::string k2 = r.read_string();
                r.expect(':');
                if (k2 == "b") {
                    r.skip_ws();
                    r.expect('{');
                    while (!r.consume('}')) {
                        std::string k3 = r.read_string();
                        r.expect(':');
                        if (k3 == "c") c = r.read_int();
                        else r.skip_value();
                        (void)r.consume(',');
                    }
                } else {
                    r.skip_value();
                }
                (void)r.consume(',');
            }
        } else {
            r.skip_value();
        }
        (void)r.consume(',');
    }
    EXPECT_EQ(c, 42);
}
