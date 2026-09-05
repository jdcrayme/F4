// test_objective_encoder.cpp — .obj encoder round-trip tests, against the
// real save1.cam fixture's .obj sub-file.
//
// The contract: encode_obj is the inverse of decode_obj.
//   decode_obj(x)  ->  d1
//   encode_obj(d1) ->  x'
//   decode_obj(x') ->  d2
//   d1 == d2   (struct equality)
//
// Struct equality is checked via encode_obj_payload identity (the same
// technique test_cmp_encoder uses: the encoder is deterministic, so equal
// structs produce equal payloads).

#include <gtest/gtest.h>
#include <f4/world_convert/cam_archive.hpp>
#include <f4/world_convert/objective_decoder.hpp>
#include <f4/world_convert/objective_encoder.hpp>
#include <f4/world_convert/lzss.hpp>
#include <f4/lzss/lzss.hpp>

#include <cstring>
#include <vector>

using namespace f4::world_convert;

namespace {

// Load the fixture's .obj sub-file and decode it.
DecodedObjectives decode_fixture_obj() {
    CamArchive cam;
    cam.load(FIXTURE_DIR "save1.cam");
    const SubFile* obj = cam.find("obj");
    EXPECT_NE(obj, nullptr);
    if (!obj) return {};
    return decode_obj(obj->data.data(), obj->data.size());
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// The golden: decode -> encode -> decode produces the identical struct.
// ═══════════════════════════════════════════════════════════════════════════

TEST(ObjEncoder, RoundTripProducesIdenticalStruct) {
    DecodedObjectives d1 = decode_fixture_obj();
    ASSERT_GT(d1.objectives.size(), 0u) << "fixture should decode objectives";

    auto encoded = encode_obj(d1);   // default camp_version=63

    ASSERT_GE(encoded.size(), 10u);
    DecodedObjectives d2;
    ASSERT_NO_THROW(d2 = decode_obj(encoded.data(), encoded.size()));

    // Struct equality via the deterministic payload encoder.
    auto p1 = encode_obj_payload(d1);
    auto p2 = encode_obj_payload(d2);
    EXPECT_EQ(p1, p2)
        << "decode->encode->decode changed the objectives";
}

TEST(ObjEncoder, RoundTripPreservesCount) {
    DecodedObjectives d1 = decode_fixture_obj();
    auto encoded = encode_obj(d1);
    DecodedObjectives d2 = decode_obj(encoded.data(), encoded.size());

    EXPECT_EQ(d2.objectives.size(), d1.objectives.size());
    EXPECT_EQ(d2.count, d1.count);
}

TEST(ObjEncoder, RoundTripPreservesObjectiveFields) {
    DecodedObjectives d1 = decode_fixture_obj();
    auto encoded = encode_obj(d1);
    DecodedObjectives d2 = decode_obj(encoded.data(), encoded.size());

    ASSERT_EQ(d2.objectives.size(), d1.objectives.size());
    // Spot-check the first objective's key fields.
    if (!d1.objectives.empty()) {
        const auto& o1 = d1.objectives[0];
        const auto& o2 = d2.objectives[0];
        EXPECT_EQ(o2.entity_type, o1.entity_type);
        EXPECT_EQ(o2.x, o1.x);
        EXPECT_EQ(o2.y, o1.y);
        EXPECT_EQ(o2.owner, o1.owner);
        EXPECT_EQ(o2.nameid, o1.nameid);
        EXPECT_EQ(o2.links, o1.links);
        EXPECT_EQ(o2.link_data.size(), o1.link_data.size());
        EXPECT_EQ(o2.fstatus, o1.fstatus);
        EXPECT_EQ(o2.has_radar, o1.has_radar);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Header structure sanity
// ═══════════════════════════════════════════════════════════════════════════

TEST(ObjEncoder, EncodedObjHasValidHeader) {
    DecodedObjectives d1 = decode_fixture_obj();
    auto encoded = encode_obj(d1);

    ASSERT_GE(encoded.size(), 10u);
    // 10-byte header: i16 count + i32 uncompressed + i32 compressed.
    int16_t count = 0; int32_t uncomp = 0, comp = 0;
    std::memcpy(&count, encoded.data(), 2);
    std::memcpy(&uncomp, encoded.data() + 2, 4);
    std::memcpy(&comp, encoded.data() + 6, 4);
    EXPECT_EQ(count, static_cast<int16_t>(d1.objectives.size()));
    EXPECT_EQ(uncomp, static_cast<int32_t>(encode_obj_payload(d1).size()));
    EXPECT_GT(comp, 0);
    EXPECT_LT(static_cast<std::size_t>(comp), encoded.size());
}

// ═══════════════════════════════════════════════════════════════════════════
// LZSS round-trip on the real .obj payload
// ═══════════════════════════════════════════════════════════════════════════

TEST(ObjEncoder, LzssRoundTripOnRealPayload) {
    // The .obj sub-file's compressed payload, decompressed and recompressed,
    // must decompress back to the identical bytes.
    CamArchive cam;
    cam.load(FIXTURE_DIR "save1.cam");
    const SubFile* obj = cam.find("obj");
    ASSERT_NE(obj, nullptr);

    // Read the original decompressed payload.
    int32_t uncomp = 0, comp = 0;
    std::memcpy(&uncomp, obj->data.data() + 2, 4);
    std::memcpy(&comp, obj->data.data() + 6, 4);
    ASSERT_GT(uncomp, 0);
    auto orig = lzss_expand(obj->data.data() + 10,
                             static_cast<std::size_t>(comp),
                             static_cast<std::size_t>(uncomp));
    ASSERT_EQ(orig.size(), static_cast<std::size_t>(uncomp));

    // Compress with the canonical f4::lzss::compress and decompress back.
    auto compressed = f4::lzss::compress(orig.data(), orig.size());
    auto roundtrip = f4::lzss::decompress(compressed.data(), compressed.size(),
                                           orig.size());
    EXPECT_EQ(roundtrip, orig)
        << "LZSS round-trip on the real .obj payload failed";
}
