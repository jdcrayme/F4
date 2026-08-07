// test_reader.cpp — f4::json::Reader

#include <gtest/gtest.h>
#include <f4/json/f4_json.hpp>
#include <clocale>      // std::setlocale
#include <stdexcept>
#include <string>
#include <vector>

using f4::json::Reader;

TEST(JsonReader, PeekConsumesWhitespace) {
    std::string s = "   {";
    Reader r(s);
    EXPECT_TRUE(r.peek('{'));
    EXPECT_EQ(r.position(), 3u);  // advanced past whitespace
}

TEST(JsonReader, ExpectAdvancesPastChar) {
    std::string s = "{}";
    Reader r(s);
    r.expect('{');
    EXPECT_EQ(r.position(), 1u);
    r.expect('}');
    EXPECT_EQ(r.position(), 2u);
}

TEST(JsonReader, ExpectThrowsOnMismatch) {
    std::string s = "{";
    Reader r(s);
    EXPECT_THROW(r.expect('}'), std::runtime_error);
}

TEST(JsonReader, ConsumeReturnsTrueAndAdvances) {
    std::string s = ", ";
    Reader r(s);
    EXPECT_TRUE(r.consume(','));
    EXPECT_EQ(r.position(), 1u);
}

TEST(JsonReader, ConsumeReturnsFalseWithoutAdvancing) {
    std::string s = "x";
    Reader r(s);
    EXPECT_FALSE(r.consume(','));
    EXPECT_EQ(r.position(), 0u);  // peek didn't advance past 'x'
}

TEST(JsonReader, ReadStringPlain) {
    std::string s = "\"hello\"";
    Reader r(s);
    EXPECT_EQ(r.read_string(), "hello");
}

TEST(JsonReader, ReadStringWithEscapes) {
    std::string s = "\"a\\nb\\tc\\\\d\\\"e\"";
    Reader r(s);
    EXPECT_EQ(r.read_string(), "a\nb\tc\\d\"e");
}

TEST(JsonReader, ReadStringWithUnicodeEscape) {
    std::string s = "\"\\u0041\"";  // 'A'
    Reader r(s);
    EXPECT_EQ(r.read_string(), "A");
}

TEST(JsonReader, ReadStringEmpty) {
    std::string s = "\"\"";
    Reader r(s);
    EXPECT_EQ(r.read_string(), "");
}

TEST(JsonReader, ReadStringThrowsOnTruncatedEscape) {
    std::string s = "\"\\u00\"";  // truncated
    Reader r(s);
    EXPECT_THROW(r.read_string(), std::runtime_error);
}

TEST(JsonReader, ReadIntPositive) {
    std::string s = "12345";
    Reader r(s);
    EXPECT_EQ(r.read_int(), 12345);
}

TEST(JsonReader, ReadIntNegative) {
    std::string s = "-42";
    Reader r(s);
    EXPECT_EQ(r.read_int(), -42);
}

TEST(JsonReader, ReadIntThrowsOnBareSign) {
    std::string s = "-";
    Reader r(s);
    EXPECT_THROW(r.read_int(), std::runtime_error);
}

TEST(JsonReader, ReadIntThrowsOnNoDigits) {
    std::string s = "abc";
    Reader r(s);
    EXPECT_THROW(r.read_int(), std::runtime_error);
}

TEST(JsonReader, ReadNumberFloat) {
    std::string s = "3.14159";
    Reader r(s);
    EXPECT_NEAR(r.read_number(), 3.14159, 1e-9);
}

TEST(JsonReader, ReadNumberScientific) {
    std::string s = "1.5e3";
    Reader r(s);
    EXPECT_NEAR(r.read_number(), 1500.0, 1e-9);
}

TEST(JsonReader, ReadNumberNegativeFloat) {
    std::string s = "-2.5e-2";
    Reader r(s);
    EXPECT_NEAR(r.read_number(), -0.025, 1e-9);
}

// Verifies that read_number() is locale-independent: a German LC_NUMERIC
// ("de_DE.UTF-8") would have caused std::strtod to parse "3.14" as 3.0
// (treating '.' as a thousand separator). std::from_chars is locale-free
// by spec, so the result must be identical in every locale.
// Skipped on systems that lack the de_DE locale installed.
TEST(JsonReader, ReadNumberLocaleIndependent) {
    const char* saved = std::setlocale(LC_NUMERIC, nullptr);
    if (std::setlocale(LC_NUMERIC, "de_DE.UTF-8") == nullptr &&
        std::setlocale(LC_NUMERIC, "de_DE")         == nullptr) {
        GTEST_SKIP() << "de_DE locale not installed; cannot verify locale-independence";
    }
    std::string s = "3.14";
    Reader r(s);
    EXPECT_NEAR(r.read_number(), 3.14, 1e-9);
    // Restore caller's locale
    std::setlocale(LC_NUMERIC, saved);
}

TEST(JsonReader, ReadIntLocaleIndependent) {
    const char* saved = std::setlocale(LC_NUMERIC, nullptr);
    if (std::setlocale(LC_NUMERIC, "de_DE.UTF-8") == nullptr &&
        std::setlocale(LC_NUMERIC, "de_DE")         == nullptr) {
        GTEST_SKIP() << "de_DE locale not installed; cannot verify locale-independence";
    }
    std::string s = "-12345";
    Reader r(s);
    EXPECT_EQ(r.read_int(), -12345LL);
    std::setlocale(LC_NUMERIC, saved);
}

TEST(JsonReader, SkipValueString) {
    std::string s = "\"skip me\" rest";
    Reader r(s);
    r.skip_value();
    EXPECT_EQ(r.position(), 9u);  // past the closing quote
}

TEST(JsonReader, SkipValueObject) {
    std::string s = "{\"a\":1,\"b\":[1,2,3]} rest";
    Reader r(s);
    r.skip_value();
    EXPECT_EQ(r.position(), 19u);  // past the closing brace
    r.skip_ws();
    EXPECT_EQ(r.position(), 20u);  // past the trailing space
}

TEST(JsonReader, SkipValueArray) {
    std::string s = "[1, 2, [3, 4], {\"x\":5}] rest";
    Reader r(s);
    r.skip_value();
    EXPECT_EQ(r.position(), 23u);  // past the closing bracket
    r.skip_ws();
    EXPECT_EQ(r.position(), 24u);  // past the trailing space
}

TEST(JsonReader, SkipValueBareToken) {
    std::string s = "true, rest";
    Reader r(s);
    r.skip_value();
    EXPECT_EQ(r.position(), 4u);  // past 'true'
}

TEST(JsonReader, SkipValueNull) {
    std::string s = "null}";
    Reader r(s);
    r.skip_value();
    EXPECT_EQ(r.position(), 4u);
}

TEST(JsonReader, SkipValueFalse) {
    std::string s = "false}";
    Reader r(s);
    r.skip_value();
    EXPECT_EQ(r.position(), 5u);
}

TEST(JsonReader, SkipValueNumber) {
    std::string s = "3.14e-2,";
    Reader r(s);
    r.skip_value();
    EXPECT_EQ(r.position(), 7u);
}

TEST(JsonReader, SkipValueRejectsMalformedBareToken) {
    // `truu` is not a valid JSON bare token. Previously the parser silently
    // swallowed any non-structural char run; now it throws.
    std::string s = "truu}";
    Reader r(s);
    EXPECT_THROW(r.skip_value(), std::runtime_error);
}

TEST(JsonReader, SkipValueRejectsBarePunctuation) {
    // `@#$%` is not a valid JSON bare token.
    std::string s = "@#$%";
    Reader r(s);
    EXPECT_THROW(r.skip_value(), std::runtime_error);
}

TEST(JsonReader, ParseSimpleObject) {
    std::string s = "{ \"name\": \"Korea\", \"width\": 128, \"active\": true }";
    Reader r(s);
    r.skip_ws();
    r.expect('{');
    std::string name;
    long long width = 0;
    while (!r.consume('}')) {
        std::string key = r.read_string();
        r.expect(':');
        if (key == "name") name = r.read_string();
        else if (key == "width") width = r.read_int();
        else r.skip_value();
        (void)r.consume(',');
    }
    EXPECT_EQ(name, "Korea");
    EXPECT_EQ(width, 128);
}

TEST(JsonReader, ParseArrayOfInts) {
    std::string s = "[1, 2, 3, 4, 5]";
    Reader r(s);
    r.skip_ws();
    r.expect('[');
    std::vector<long long> out;
    if (!r.peek(']')) {
        for (;;) {
            out.push_back(r.read_int());
            if (r.consume(']')) break;
            r.expect(',');
        }
    }
    EXPECT_EQ(out.size(), 5u);
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[4], 5);
}

TEST(JsonReader, ParseEmptyObject) {
    std::string s = "{}";
    Reader r(s);
    r.skip_ws();
    r.expect('{');
    EXPECT_TRUE(r.consume('}'));  // immediate close
}

TEST(JsonReader, ParseEmptyArray) {
    std::string s = "[]";
    Reader r(s);
    r.skip_ws();
    r.expect('[');
    EXPECT_TRUE(r.consume(']'));
}
