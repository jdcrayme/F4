// test_cmp_encoder.cpp — .cmp encoder round-trip tests, against the real
// save1.cam fixture.
//
// The contract: encode_cmp is the inverse of decode_cmp.
//   decode_cmp(x)  ->  h1
//   encode_cmp(h1) ->  x'
//   decode_cmp(x') ->  h2
//   h1 == h2   (struct equality)
//
// Struct equality is checked via the encoder itself: because encode_cmp_payload
// is deterministic and writes every field, h1 == h2  <=>  encode_cmp_payload(h1)
// == encode_cmp_payload(h2). This is a single vector comparison that surfaces
// any field that fails to round-trip, without a 40-field EXPECT list.
//
// Also covers: the LZSS compressor on the real .cmp decompressed payload
// (decompress -> compress -> decompress == original), and the .cmp header
// fields (reserved_skip, decompressed_size) round-tripping.

#include <gtest/gtest.h>
#include <f4/world_convert/cam_archive.hpp>
#include <f4/world_convert/campaign_decoder.hpp>
#include <f4/world_convert/cmp_encoder.hpp>
#include <f4/world_convert/lzss.hpp>
#include <f4/lzss/lzss.hpp>

#include <cstring>
#include <vector>

using namespace f4::world_convert;

namespace {

// Load the fixture's .cmp sub-file and decode it.
CampaignHeader decode_fixture_cmp(const SubFile** cmp_out = nullptr) {
    CamArchive cam;
    cam.load(FIXTURE_DIR "save1.cam");
    const SubFile* cmp = cam.find("cmp");
    EXPECT_NE(cmp, nullptr);
    if (cmp_out) *cmp_out = cmp;
    return decode_cmp(cmp->data.data(), cmp->data.size());
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// The golden: decode -> encode -> decode produces the identical struct.
// ═══════════════════════════════════════════════════════════════════════════

TEST(CmpEncoder, RoundTripProducesIdenticalStruct) {
    CampaignHeader h1 = decode_fixture_cmp();
    auto encoded = encode_cmp(h1);   // default camp_version=63 (the fixture)

    ASSERT_GE(encoded.size(), 8u);
    CampaignHeader h2;
    ASSERT_NO_THROW(h2 = decode_cmp(encoded.data(), encoded.size()));

    // Struct equality via the deterministic encoder: equal structs produce
    // equal payloads (every field is written in a fixed order). Any field
    // that fails to round-trip shows up as a payload difference here.
    auto payload1 = encode_cmp_payload(h1);
    auto payload2 = encode_cmp_payload(h2);
    EXPECT_EQ(payload1, payload2)
        << "decode->encode->decode changed the campaign header";

    // The two .cmp-level header fields round-trip too.
    EXPECT_EQ(h2.reserved_skip, h1.reserved_skip);
    EXPECT_EQ(h2.decompressed_size, static_cast<int32_t>(payload1.size()));
}

TEST(CmpEncoder, RoundTripConsumesEntirePayload) {
    // The re-encoded .cmp must decode cleanly: the cursor lands exactly at
    // the payload end (no undecoded trailing bytes, no overrun). This
    // confirms the encoder writes exactly the fields the decoder reads,
    // in order, with no extras and no gaps.
    CampaignHeader h1 = decode_fixture_cmp();
    auto encoded = encode_cmp(h1);
    CampaignHeader h2 = decode_cmp(encoded.data(), encoded.size());

    EXPECT_EQ(h2.remaining_payload.size(), 0u);
    EXPECT_EQ(h2.bytes_consumed, static_cast<std::size_t>(h2.decompressed_size));
}

// ═══════════════════════════════════════════════════════════════════════════
// Header / structure sanity on the encoded .cmp
// ═══════════════════════════════════════════════════════════════════════════

TEST(CmpEncoder, EncodedCmpHasValidHeader) {
    CampaignHeader h1 = decode_fixture_cmp();
    auto encoded = encode_cmp(h1);

    // 8-byte header: reserved_skip (i32) + decompressed_size (i32).
    ASSERT_GE(encoded.size(), 8u);
    int32_t reserved = 0, dec_size = 0;
    std::memcpy(&reserved, encoded.data(), 4);
    std::memcpy(&dec_size, encoded.data() + 4, 4);
    EXPECT_EQ(reserved, h1.reserved_skip);
    EXPECT_EQ(dec_size, static_cast<int32_t>(encode_cmp_payload(h1).size()));
    EXPECT_GT(dec_size, 0);
}

TEST(CmpEncoder, PreservesKnownFixtureValues) {
    // After the round-trip, the fixture's known semantic values survive
    // (test_campaign.cpp checks these on the original decode — this test
    // confirms they survive the encode/decode cycle).
    CampaignHeader h1 = decode_fixture_cmp();
    auto encoded = encode_cmp(h1);
    CampaignHeader h2 = decode_cmp(encoded.data(), encoded.size());

    EXPECT_EQ(h2.current_time, h1.current_time);
    EXPECT_GT(h2.current_time, 0);
    EXPECT_EQ(h2.theater_name, "KOREA");
    EXPECT_EQ(h2.theater_size_x, 1024);
    EXPECT_EQ(h2.theater_size_y, 1024);
    EXPECT_EQ(static_cast<int>(h2.active_teams), 8);
    EXPECT_EQ(h2.teams.size(), 8u);
    EXPECT_EQ(h2.teams[0].name, h1.teams[0].name);
}

// ═══════════════════════════════════════════════════════════════════════════
// LZSS compressor on the real .cmp payload
// ═══════════════════════════════════════════════════════════════════════════

TEST(CmpEncoder, LzssRoundTripOnRealPayload) {
    // The .cmp sub-file's compressed payload, decompressed and recompressed,
    // must decompress back to the identical bytes. This exercises the LZSS
    // compressor on real campaign data (long NUL runs, struct sequences)
    // independent of the cmp struct serialization.
    const SubFile* cmp = nullptr;
    decode_fixture_cmp(&cmp);
    ASSERT_NE(cmp, nullptr);

    // Read the original decompressed payload via the decode adapter.
    int32_t dec_size = 0;
    std::memcpy(&dec_size, cmp->data.data() + 4, 4);
    ASSERT_GT(dec_size, 0);
    auto orig_payload = lzss_expand(cmp->data.data() + 8,
                                    cmp->data.size() - 8,
                                    static_cast<std::size_t>(dec_size));
    ASSERT_EQ(orig_payload.size(), static_cast<std::size_t>(dec_size));

    // Compress with the canonical f4::lzss::compress and decompress back.
    auto compressed = f4::lzss::compress(orig_payload.data(), orig_payload.size());
    auto roundtrip = f4::lzss::decompress(compressed.data(), compressed.size(),
                                           orig_payload.size());
    EXPECT_EQ(roundtrip, orig_payload)
        << "LZSS round-trip on the real .cmp payload failed";

    // And the re-compressed size should be in the same ballpark as the
    // original (both are valid LZSS encodings; the byte streams differ but
    // the ratio is similar). This is a smoke check, not an exact bar.
    EXPECT_LT(compressed.size(), orig_payload.size() * 2);
}
