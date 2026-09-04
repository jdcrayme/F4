// test_unit_encoder.cpp — .uni encoder round-trip tests, against the real
// save1.cam fixture's .uni sub-file.
//
// The contract: encode_uni is the inverse of decode_uni.
//   decode_uni(x)  ->  d1
//   encode_uni(d1) ->  x'
//   decode_uni(x') ->  d2
//   d1 == d2   (struct equality)
//
// Struct equality is checked via encode_uni_payload identity (the encoder is
// deterministic, so equal structs produce equal payloads). This is the same
// technique as test_cmp_encoder / test_objective_encoder.

#include <gtest/gtest.h>
#include <f4/world_convert/cam_archive.hpp>
#include <f4/world_convert/unit_decoder.hpp>
#include <f4/world_convert/unit_encoder.hpp>
#include <f4/world_convert/class_table.hpp>
#include <f4/world_convert/lzss.hpp>
#include <f4/lzss/lzss.hpp>

#include <cstring>
#include <vector>

using namespace f4::world_convert;

namespace {

// Load the fixture's .uni sub-file and decode it (with the class table for
// deterministic dispatch — the repo bundles FALCON4.ct).
DecodedUnits decode_fixture_uni() {
    CamArchive cam;
    cam.load(FIXTURE_DIR "save1.cam");
    const SubFile* uni = cam.find("uni");
    EXPECT_NE(uni, nullptr);
    if (!uni) return {};

    // Load the class table for deterministic subclass dispatch.
    ClassTable ct;
    try {
        ct.load(FIXTURE_DIR "FALCON4.ct");
    } catch (...) {
        // If the class table can't load, proceed without it (trial-and-
        // error dispatch). The round-trip still works; it's just slower.
    }

    UnitDecodeOptions opts;
    opts.camp_version = 63;
    opts.class_table = ct.size() > 0 ? &ct : nullptr;
    return decode_uni(uni->data.data(), uni->data.size(), opts);
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// The golden: decode -> encode -> decode produces the identical struct.
// ═══════════════════════════════════════════════════════════════════════════

TEST(UniEncoder, RoundTripProducesIdenticalStruct) {
    DecodedUnits d1 = decode_fixture_uni();
    ASSERT_GT(d1.units.size(), 0u) << "fixture should decode units";

    auto encoded = encode_uni(d1);   // default camp_version=63

    ASSERT_GE(encoded.size(), 10u);

    // Re-decode with the same options.
    ClassTable ct;
    try { ct.load(FIXTURE_DIR "FALCON4.ct"); } catch (...) {}
    UnitDecodeOptions opts;
    opts.camp_version = 63;
    opts.class_table = ct.size() > 0 ? &ct : nullptr;

    DecodedUnits d2;
    ASSERT_NO_THROW(d2 = decode_uni(encoded.data(), encoded.size(), opts));

    // Struct equality via the deterministic payload encoder.
    auto p1 = encode_uni_payload(d1);
    auto p2 = encode_uni_payload(d2);
    EXPECT_EQ(p1, p2)
        << "decode->encode->decode changed the units";
}

TEST(UniEncoder, RoundTripPreservesCount) {
    DecodedUnits d1 = decode_fixture_uni();
    ClassTable ct;
    try { ct.load(FIXTURE_DIR "FALCON4.ct"); } catch (...) {}
    UnitDecodeOptions opts;
    opts.camp_version = 63;
    opts.class_table = ct.size() > 0 ? &ct : nullptr;

    auto encoded = encode_uni(d1);
    DecodedUnits d2 = decode_uni(encoded.data(), encoded.size(), opts);

    EXPECT_EQ(d2.units.size(), d1.units.size());
    EXPECT_EQ(d2.count, d1.count);
}

TEST(UniEncoder, RoundTripPreservesSubclassDistribution) {
    // The fixture has a mix of battalions, squadrons, flights, etc.
    // The round-trip must preserve each unit's subclass.
    DecodedUnits d1 = decode_fixture_uni();
    ClassTable ct;
    try { ct.load(FIXTURE_DIR "FALCON4.ct"); } catch (...) {}
    UnitDecodeOptions opts;
    opts.camp_version = 63;
    opts.class_table = ct.size() > 0 ? &ct : nullptr;

    auto encoded = encode_uni(d1);
    DecodedUnits d2 = decode_uni(encoded.data(), encoded.size(), opts);

    ASSERT_EQ(d2.units.size(), d1.units.size());
    for (std::size_t i = 0; i < d1.units.size(); ++i) {
        EXPECT_EQ(d2.units[i].unit_class, d1.units[i].unit_class)
            << "unit " << i << " subclass changed: "
            << unit_class_name(d1.units[i].unit_class) << " -> "
            << unit_class_name(d2.units[i].unit_class);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Per-subclass spot-checks
// ═══════════════════════════════════════════════════════════════════════════

TEST(UniEncoder, RoundTripPreservesBattalionFields) {
    DecodedUnits d1 = decode_fixture_uni();
    ClassTable ct;
    try { ct.load(FIXTURE_DIR "FALCON4.ct"); } catch (...) {}
    UnitDecodeOptions opts;
    opts.camp_version = 63;
    opts.class_table = ct.size() > 0 ? &ct : nullptr;
    auto encoded = encode_uni(d1);
    DecodedUnits d2 = decode_uni(encoded.data(), encoded.size(), opts);

    // Find the first battalion.
    std::size_t idx = 0;
    for (; idx < d1.units.size(); ++idx) {
        if (d1.units[idx].unit_class == UnitClass::Battalion) break;
    }
    if (idx < d1.units.size()) {
        const auto& u1 = d1.units[idx];
        const auto& u2 = d2.units[idx];
        EXPECT_EQ(u2.type, u1.type);
        EXPECT_EQ(u2.x, u1.x);
        EXPECT_EQ(u2.y, u1.y);
        EXPECT_EQ(u2.owner, u1.owner);
        EXPECT_EQ(u2.subclass.orders, u1.subclass.orders);
        EXPECT_EQ(u2.subclass.division, u1.subclass.division);
        EXPECT_EQ(u2.subclass.supply, u1.subclass.supply);
        EXPECT_EQ(u2.subclass.fatigue, u1.subclass.fatigue);
        EXPECT_EQ(u2.subclass.morale, u1.subclass.morale);
    }
}

TEST(UniEncoder, RoundTripPreservesSquadronFields) {
    DecodedUnits d1 = decode_fixture_uni();
    ClassTable ct;
    try { ct.load(FIXTURE_DIR "FALCON4.ct"); } catch (...) {}
    UnitDecodeOptions opts;
    opts.camp_version = 63;
    opts.class_table = ct.size() > 0 ? &ct : nullptr;
    auto encoded = encode_uni(d1);
    DecodedUnits d2 = decode_uni(encoded.data(), encoded.size(), opts);

    std::size_t idx = 0;
    for (; idx < d1.units.size(); ++idx) {
        if (d1.units[idx].unit_class == UnitClass::Squadron) break;
    }
    if (idx < d1.units.size()) {
        const auto& u1 = d1.units[idx];
        const auto& u2 = d2.units[idx];
        EXPECT_EQ(u2.subclass.fuel, u1.subclass.fuel);
        EXPECT_EQ(u2.subclass.specialty, u1.subclass.specialty);
        EXPECT_EQ(u2.subclass.pilots.size(), u1.subclass.pilots.size());
        if (u1.subclass.pilots.size() == u2.subclass.pilots.size() &&
            !u1.subclass.pilots.empty()) {
            EXPECT_EQ(u2.subclass.pilots[0].pilot_id, u1.subclass.pilots[0].pilot_id);
            EXPECT_EQ(u2.subclass.pilots[0].aa_kills, u1.subclass.pilots[0].aa_kills);
        }
        EXPECT_EQ(u2.subclass.airbase_id_num, u1.subclass.airbase_id_num);
        EXPECT_EQ(u2.subclass.aa_kills, u1.subclass.aa_kills);
        EXPECT_EQ(u2.subclass.squadron_patch, u1.subclass.squadron_patch);
    }
}

TEST(UniEncoder, RoundTripPreservesFlightFields) {
    DecodedUnits d1 = decode_fixture_uni();
    ClassTable ct;
    try { ct.load(FIXTURE_DIR "FALCON4.ct"); } catch (...) {}
    UnitDecodeOptions opts;
    opts.camp_version = 63;
    opts.class_table = ct.size() > 0 ? &ct : nullptr;
    auto encoded = encode_uni(d1);
    DecodedUnits d2 = decode_uni(encoded.data(), encoded.size(), opts);

    std::size_t idx = 0;
    for (; idx < d1.units.size(); ++idx) {
        if (d1.units[idx].unit_class == UnitClass::Flight) break;
    }
    if (idx < d1.units.size()) {
        const auto& u1 = d1.units[idx];
        const auto& u2 = d2.units[idx];
        EXPECT_EQ(u2.subclass.fuel_burnt, u1.subclass.fuel_burnt);
        EXPECT_EQ(u2.subclass.mission, u1.subclass.mission);
        EXPECT_EQ(u2.subclass.loadouts, u1.subclass.loadouts);
        EXPECT_EQ(u2.subclass.loadout_stations.size(),
                  u1.subclass.loadout_stations.size());
        EXPECT_EQ(u2.subclass.package_num, u1.subclass.package_num);
        EXPECT_EQ(u2.subclass.squadron_num, u1.subclass.squadron_num);
        EXPECT_EQ(u2.subclass.callsign_id, u1.subclass.callsign_id);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Header structure sanity
// ═══════════════════════════════════════════════════════════════════════════

TEST(UniEncoder, EncodedUniHasValidHeader) {
    DecodedUnits d1 = decode_fixture_uni();
    auto encoded = encode_uni(d1);

    ASSERT_GE(encoded.size(), 10u);
    // 10-byte header: i32 outer + i16 count + i32 inner.
    int32_t outer = 0, inner = 0; int16_t count = 0;
    std::memcpy(&outer, encoded.data(), 4);
    std::memcpy(&count, encoded.data() + 4, 2);
    std::memcpy(&inner, encoded.data() + 6, 4);
    EXPECT_EQ(count, static_cast<int16_t>(d1.units.size()));
    EXPECT_EQ(inner, static_cast<int32_t>(encode_uni_payload(d1).size()));
    EXPECT_GT(outer, 0);
    EXPECT_GE(static_cast<std::size_t>(outer), encoded.size());
}

// ═══════════════════════════════════════════════════════════════════════════
// LZSS round-trip on the real .uni payload
// ═══════════════════════════════════════════════════════════════════════════

TEST(UniEncoder, LzssRoundTripOnRealPayload) {
    CamArchive cam;
    cam.load(FIXTURE_DIR "save1.cam");
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);

    int32_t inner = 0, outer = 0; int16_t count = 0;
    std::memcpy(&outer, uni->data.data(), 4);
    std::memcpy(&count, uni->data.data() + 4, 2);
    std::memcpy(&inner, uni->data.data() + 6, 4);
    ASSERT_GT(inner, 0);

    auto orig = lzss_expand(uni->data.data() + 10,
                             uni->data.size() - 10,
                             static_cast<std::size_t>(inner));
    ASSERT_EQ(orig.size(), static_cast<std::size_t>(inner));

    auto compressed = f4::lzss::compress(orig.data(), orig.size());
    auto roundtrip = f4::lzss::decompress(compressed.data(), compressed.size(),
                                           orig.size());
    EXPECT_EQ(roundtrip, orig)
        << "LZSS round-trip on the real .uni payload failed";
}
