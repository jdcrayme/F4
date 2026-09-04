// f4-xml/tests/test_xml.cpp
//
// Smoke tests for the vendored pugixml build + the f4::xml alias. pugixml
// itself is upstream-tested; these prove the target compiles/links on the
// project's toolchains and pin the usage patterns f4 code relies on
// (load_string, attribute queries, child iteration, node text, save).

#include <f4/xml/f4_xml.hpp>

#include <gtest/gtest.h>

#include <sstream>
#include <string>

namespace {

TEST(XmlSmoke, ParsesDocumentAndQueriesNodes) {
    // Note the "xml" delimiter: the content contains ')"' (inside
    // translate(...)) which would terminate a plain R"(...)" literal.
    const std::string src = R"xml(
        <svg xmlns="http://www.w3.org/2000/svg" viewBox="-1 -1 2 2">
            <title>Test</title>
            <g transform="translate(1,2)">
                <path d="M 0 0 L 1 1 Z" fill="currentColor"/>
            </g>
        </svg>
    )xml";
    f4::xml::xml_document doc;
    const f4::xml::xml_parse_result result = doc.load_string(src.c_str());
    ASSERT_TRUE(result) << result.description();

    const f4::xml::xml_node svg = doc.child("svg");
    ASSERT_TRUE(svg);
    EXPECT_EQ(std::string(svg.attribute("viewBox").as_string()), "-1 -1 2 2");
    EXPECT_EQ(std::string(svg.child_value("title")), "Test");

    const f4::xml::xml_node g = svg.child("g");
    ASSERT_TRUE(g);
    EXPECT_EQ(std::string(g.attribute("transform").as_string()), "translate(1,2)");

    const f4::xml::xml_node path = g.child("path");
    ASSERT_TRUE(path);
    EXPECT_EQ(std::string(path.attribute("d").as_string()), "M 0 0 L 1 1 Z");
    EXPECT_EQ(std::string(path.attribute("fill").as_string()), "currentColor");
}

TEST(XmlSmoke, IteratesChildrenOfOneName) {
    f4::xml::xml_document doc;
    ASSERT_TRUE(doc.load_string("<r><i a=\"1\"/><i a=\"2\"/><other/></r>"));

    int count = 0;
    std::string values;
    for (f4::xml::xml_node n = doc.child("r").child("i"); n;
         n = n.next_sibling("i")) {
        values += n.attribute("a").as_string();
        ++count;
    }
    EXPECT_EQ(count, 2);
    EXPECT_EQ(values, "12");
}

TEST(XmlSmoke, ReportsParseErrorsWithOffset) {
    f4::xml::xml_document doc;
    const f4::xml::xml_parse_result result = doc.load_string("<a><b></a>");
    EXPECT_FALSE(result);
    EXPECT_NE(result.offset, 0u);
}

TEST(XmlSmoke, RoundTripsThroughSave) {
    f4::xml::xml_document doc;
    ASSERT_TRUE(doc.load_string("<r><e k=\"v\">text</e></r>"));

    std::ostringstream ss;
    doc.save(ss /* default: pretty-print with declaration */);
    const std::string saved = ss.str();

    f4::xml::xml_document doc2;
    ASSERT_TRUE(doc2.load_string(saved.c_str()));
    EXPECT_EQ(std::string(doc2.child("r").child("e").attribute("k").as_string()),
              "v");
    EXPECT_EQ(std::string(doc2.child("r").child("e").child_value()), "text");
}

TEST(XmlSmoke, BuildsNodesProgrammatically) {
    // The SVG exporter builds documents node-by-node; pin that pattern.
    f4::xml::xml_document doc;
    f4::xml::xml_node svg = doc.append_child("svg");
    svg.append_attribute("viewBox").set_value("-1 -1 2 2");
    f4::xml::xml_node path = svg.append_child("path");
    path.append_attribute("d").set_value("M 0 0 L 1 1");
    path.append_attribute("fill").set_value("none");

    std::ostringstream ss;
    doc.save(ss, "", f4::xml::format_raw);
    EXPECT_NE(ss.str().find(R"(<svg viewBox="-1 -1 2 2">)"), std::string::npos);
    EXPECT_NE(ss.str().find(R"(<path d="M 0 0 L 1 1" fill="none"/>)"),
              std::string::npos);
}

} // namespace
