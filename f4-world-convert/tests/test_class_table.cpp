// test_class_table.cpp — ClassTable parser tests.
//
// Verifies the visType[7] exposure added when closing the aircraft-binding
// data-flow gap (see Docs/SCENARIO_PLAYER_PLAN.md §4.1). Previously the
// parser read only classInfo_[4] + dataType + dataPtr from each 81-byte
// record, silently discarding the 14-byte visType[7] array at offset 60.
// That left no path from entity_type → visual model index without which
// VisualModelComponent cannot be auto-resolved from campaign data.
//
// Tests against the bundled FALCON4.ct fixture (2135 entries) — the same
// file the cam2json CLI uses. We assert specific known values for the
// F-16's CT entry to lock in the on-disk format semantics.

#include <gtest/gtest.h>
#include <f4/world_convert/class_table.hpp>
#include <f4/json/writer.hpp>

#include <filesystem>
#include <fstream>

using namespace f4::world_convert;

namespace {
ClassTable load_fixture() {
    ClassTable ct;
    ct.load(FIXTURE_DIR "FALCON4.ct");
    return ct;
}
}  // namespace

TEST(ClassTable, LoadsFixtureWithExpectedEntryCount) {
    auto ct = load_fixture();
    // The bundled FALCON4.ct has 2135 entries (entity_type 100..2234).
    // If this regesses, the fixture was replaced.
    EXPECT_EQ(ct.size(), 2135u);
}

TEST(ClassTable, ExistingFieldsStillParseCorrectly) {
    // Regression guard: the visType[7] addition must not have broken
    // parsing of the previously-exposed classInfo_[4] / dataType / dataPtr.
    // Pick an entity_type we know has a UNIT data pointer — entity_type
    // 378 is DOMAIN_AIR / CLASS_UNIT / TYPE_SQUADRON / STYPE_FIGHTER
    // (a fighter squadron). data_type should be DTYPE_UNIT (4).
    auto ct = load_fixture();
    uint8_t data_type = 0;
    uint32_t data_ptr = 0;
    ASSERT_TRUE(ct.data_ptr_for(378, data_type, data_ptr));
    EXPECT_EQ(data_type, DTYPE_UNIT);
    // data_ptr is an index into Falcon4.UCD; we don't pin the exact value
    // (it varies by Falcon4 version) — just assert it's > 0.
    EXPECT_GT(data_ptr, 0u);
}

TEST(ClassTable, VisTypeArrayIsExposedOnEntry) {
    // The struct field itself must exist and be readable. This catches
    // a regression where someone reverts the field addition but leaves
    // the accessor in place (the linker would catch it, but the test
    // message is more helpful than a link error).
    //
    // Use entity_type 273 — a CLASS_VEHICLE / DOMAIN_AIR entry with
    // vis_type[0] = 1052 (the F-16 aircraft model).
    auto ct = load_fixture();
    const auto* e = ct.lookup(273);
    ASSERT_NE(e, nullptr);
    // visType is a 7-element array. We just verify we can read all 7
    // without crashing; specific values are checked in the next test.
    for (int i = 0; i < 7; ++i) {
        // No assertion — just exercising the field access.
        (void)e->vis_type[i];
    }
}

TEST(ClassTable, F16SquadronHasVisTypePointer) {
    // entity_type 273 is a CLASS_VEHICLE / DOMAIN_AIR entry whose
    // vis_type[0] is 1052 — the index of the F-16 aircraft model in
    // KoreaObj.HDR. This is the data path that VisualModelComponent
    // uses (via ModelDatabase::model(vis_type[0])) to resolve the
    // renderable for an aircraft spawned from campaign data.
    //
    // We pin vis_type[0] == 1052 because that's the value in the bundled
    // fixture. If a future fixture update changes this, the test should
    // be updated to match — silently pointing at the wrong model would
    // be worse than a test failure.
    //
    // Contrast: entity_type 1052 is a CLASS_FEATURE (not a vehicle) and
    // has vis_type[0] = 1050 — a different model entirely. The CT maps
    // many entity_types to many models; do not assume entity_type ==
    // vis_type.
    auto ct = load_fixture();
    const auto v0 = ct.vis_type_for(273, 0);
    EXPECT_EQ(v0, 1052);
}

TEST(ClassTable, VisTypeSlotOutOfBoundsReturnsZero) {
    auto ct = load_fixture();
    EXPECT_EQ(ct.vis_type_for(1052, -1), 0);
    EXPECT_EQ(ct.vis_type_for(1052, 7), 0);
    EXPECT_EQ(ct.vis_type_for(1052, 99), 0);
}

TEST(ClassTable, VisTypeForUnknownEntityTypeReturnsZero) {
    auto ct = load_fixture();
    EXPECT_EQ(ct.vis_type_for(50, 0), 0);                          // below VU_LAST_ENTITY_TYPE (100)
    EXPECT_EQ(ct.vis_type_for(static_cast<uint16_t>(70000), 0), 0); // above the table's range (wraps to 4464)
}

TEST(ClassTable, MultipleEntityTypesPointAtF16AircraftModel) {
    // Sanity check: the F-16 aircraft model (KoreaObj.HDR index 1052)
    // is referenced by multiple VEHICLE-class entity_types (different
    // loadouts / squadrons / countries). This is the inverse of the
    // F16SquadronHasVisTypePointer test — we look for entity_types
    // whose vis_type[0] == 1052.
    auto ct = load_fixture();
    int match_count = 0;
    for (std::size_t i = 0; i < ct.size(); ++i) {
        const auto et = static_cast<uint16_t>(100 + i);
        if (ct.vis_type_for(et, 0) == 1052) {
            ++match_count;
        }
    }
    // The bundled fixture has at least 4 VEHICLE-class entity_types
    // pointing at model 1052 (entity_types 273, 276, 285, 719, ...).
    // We assert >= 3 to be tolerant of fixture version drift.
    EXPECT_GE(match_count, 3);
}

TEST(ClassTable, VisTypeArrayHasNonZeroEntriesForMostRecords) {
    // Sanity check: the parser isn't just returning zeros for everything.
    // The bundled fixture has ~1080 entries with vis_type[0] != 0 (about
    // half the table — the other half are abstract / manager classes
    // with no renderable). We assert >= 500 to be tolerant of fixture
    // version drift while still catching a "everything is zero" regression.
    auto ct = load_fixture();
    int non_zero = 0;
    for (std::size_t i = 0; i < ct.size(); ++i) {
        const auto et = static_cast<uint16_t>(100 + i);
        if (ct.vis_type_for(et, 0) != 0) ++non_zero;
    }
    EXPECT_GE(non_zero, 500);
}

// ============================================================================
// Tranche 0a.1 (NO_BINARY_RUNTIME_PLAN.md): ClassTable::load_json round-trip.
// ct2json produces falcon4.ct.json from FALCON4.ct; load_json reads it back.
// The two ClassTable instances must be identical — behavior-preserving.
// ============================================================================
TEST(ClassTable, LoadJsonRoundTripsIdenticalToBinaryLoad) {
    // Load the binary form (the source of truth).
    ClassTable ct_bin;
    ct_bin.load(FIXTURE_DIR "FALCON4.ct");
    ASSERT_TRUE(ct_bin.loaded());
    const auto n = ct_bin.size();
    ASSERT_GT(n, 0u);

    // Write the JSON form to a temp file via the ct2json library path.
    // (We don't shell out to the CLI in a unit test; we emit the JSON
    // inline using the same writer pattern, then load_json it back.)
    f4::json::Writer w;
    w.raw("{\n");
    w.string("format"); w.raw(":"); w.string("f4-class-table"); w.raw(",\n");
    w.string("version"); w.raw(":"); w.number(1); w.raw(",\n");
    w.string("source"); w.raw(":"); w.string("FALCON4.ct"); w.raw(",\n");
    w.string("source_fingerprint"); w.raw(":"); w.string("test"); w.raw(",\n");
    w.string("first_entity_type"); w.raw(":"); w.number(100); w.raw(",\n");
    w.number_key("count", static_cast<std::uint64_t>(n)); w.raw(",\n");
    w.string("entries"); w.raw(": [\n");
    for (std::size_t i = 0; i < n; ++i) {
        const auto* e = ct_bin.lookup(static_cast<uint16_t>(100 + i));
        ASSERT_NE(e, nullptr);
        w.raw("  {");
        w.number_key("entity_type", static_cast<int>(100 + i)); w.raw(", ");
        w.number_key("domain", static_cast<int>(e->domain)); w.raw(", ");
        w.number_key("cls", static_cast<int>(e->cls)); w.raw(", ");
        w.number_key("type", static_cast<int>(e->type)); w.raw(", ");
        w.number_key("stype", static_cast<int>(e->stype)); w.raw(", ");
        w.raw("\"vis_type\": [");
        for (int v = 0; v < 7; ++v) {
            if (v) w.raw(", ");
            w.number(static_cast<int>(e->vis_type[v]));
        }
        w.raw("], ");
        w.number_key("data_type", static_cast<int>(e->data_type)); w.raw(", ");
        w.number_key("data_ptr_index", static_cast<uint32_t>(e->data_ptr_index));
        w.raw("}");
        if (i + 1 < n) w.raw(",");
        w.raw("\n");
    }
    w.raw("]\n}\n");

    const auto tmp = std::filesystem::temp_directory_path() / "f4_ct_roundtrip.json";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << w.str();
    }

    // Load the JSON form back.
    ClassTable ct_json;
    ct_json.load_json(tmp);
    ASSERT_TRUE(ct_json.loaded());
    ASSERT_EQ(ct_json.size(), n);

    // Every entry must match.
    for (std::size_t i = 0; i < n; ++i) {
        const auto et = static_cast<uint16_t>(100 + i);
        const auto* a = ct_bin.lookup(et);
        const auto* b = ct_json.lookup(et);
        ASSERT_NE(a, nullptr) << "binary lookup failed at index " << i;
        ASSERT_NE(b, nullptr) << "json lookup failed at index " << i;
        EXPECT_EQ(a->domain, b->domain) << "domain mismatch at entity_type " << et;
        EXPECT_EQ(a->cls, b->cls) << "cls mismatch at entity_type " << et;
        EXPECT_EQ(a->type, b->type) << "type mismatch at entity_type " << et;
        EXPECT_EQ(a->stype, b->stype) << "stype mismatch at entity_type " << et;
        EXPECT_EQ(a->data_type, b->data_type) << "data_type mismatch at entity_type " << et;
        EXPECT_EQ(a->data_ptr_index, b->data_ptr_index) << "data_ptr_index mismatch at entity_type " << et;
        for (int v = 0; v < 7; ++v) {
            EXPECT_EQ(a->vis_type[v], b->vis_type[v])
                << "vis_type[" << v << "] mismatch at entity_type " << et;
        }
    }

    // The derived lookups must also match (they read the same fields).
    for (std::size_t i = 0; i < n; ++i) {
        const auto et = static_cast<uint16_t>(100 + i);
        EXPECT_EQ(ct_bin.objective_type_for(et), ct_json.objective_type_for(et));
        EXPECT_EQ(ct_bin.unit_subtype_for(et), ct_json.unit_subtype_for(et));
        EXPECT_EQ(ct_bin.vis_type_for(et, 0), ct_json.vis_type_for(et, 0));
        uint8_t dt_bin = 0, dt_json = 0;
        uint32_t dp_bin = 0, dp_json = 0;
        const bool has_bin = ct_bin.data_ptr_for(et, dt_bin, dp_bin);
        const bool has_json = ct_json.data_ptr_for(et, dt_json, dp_json);
        EXPECT_EQ(has_bin, has_json) << "data_ptr_for presence differs at entity_type " << et;
        if (has_bin) {
            EXPECT_EQ(dt_bin, dt_json) << "data_type mismatch at entity_type " << et;
            EXPECT_EQ(dp_bin, dp_json) << "data_ptr_index mismatch at entity_type " << et;
        }
    }

    std::filesystem::remove(tmp);
}
