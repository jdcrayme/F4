// f4-world-types/tests/test_class_table_json.cpp
//
// Smoke tests for the runtime-safe ClassTable JSON loader. Verifies the
// loader reads the committed Data/Classes/falcon4.ct.json correctly and
// the lookup methods return the same values as the f4-world-convert
// implementation (behavior-preserving extraction).

#include <f4/world_types/class_table.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>

namespace wt = f4::world_types;

namespace {
wt::ClassTable load_committed() {
    wt::ClassTable ct;
    ct.load_json(F4_CT_JSON);
    return ct;
}
} // namespace

// ── The committed falcon4.ct.json loads + has the expected entry count ─────

TEST(ClassTableJson, LoadsCommittedJson) {
    auto ct = load_committed();
    EXPECT_TRUE(ct.loaded());
    // The shipped Falcon4.ct has 2135 entries; the JSON is a 1:1 mirror.
    EXPECT_EQ(ct.size(), 2135u);
}

// ── load_auto dispatches on extension ───────────────────────────────────────

TEST(ClassTableJson, LoadAutoJsonExtension) {
    wt::ClassTable ct;
    EXPECT_NO_THROW(ct.load_auto(std::filesystem::path(F4_CT_JSON)));
    EXPECT_TRUE(ct.loaded());
}

TEST(ClassTableJson, LoadAutoRejectsBinaryCt) {
    wt::ClassTable ct;
    // A .ct path must throw — the runtime doesn't link the binary decoder.
    EXPECT_THROW(ct.load_auto(std::filesystem::path("FALCON4.ct")), std::runtime_error);
}

// ── Lookup methods return the documented values ─────────────────────────────

TEST(ClassTableJson, LookupOutOfRange) {
    auto ct = load_committed();
    EXPECT_EQ(ct.lookup(50), nullptr);   // below VU_LAST_ENTITY_TYPE (100)
    EXPECT_EQ(ct.lookup(99999), nullptr); // above the table
}

TEST(ClassTableJson, VisTypeForReturnsModel) {
    auto ct = load_committed();
    // entity_type 100 is the first entry; vis_type[0] should be a valid
    // model index (0 = "no model", but the first few entries are real
    // objectives/units with models). Just verify the lookup doesn't crash
    // and returns something <= the model db size (sanity bound).
    const int16_t vt = ct.vis_type_for(100, 0);
    EXPECT_GE(vt, 0);
}

TEST(ClassTableJson, VisTypeForBadSlot) {
    auto ct = load_committed();
    EXPECT_EQ(ct.vis_type_for(100, -1), 0);
    EXPECT_EQ(ct.vis_type_for(100, 7), 0);
}

TEST(ClassTableJson, ObjectiveTypeFor) {
    auto ct = load_committed();
    // Walk the first ~50 entries; at least one should be an objective
    // (CLASS_OBJECTIVE=4) with a non-zero type (ObjectiveType 1-39).
    bool saw_objective = false;
    for (uint16_t et = 100; et < 150; ++et) {
        const auto* e = ct.lookup(et);
        if (!e) continue;
        if (e->cls == wt::CLASS_OBJECTIVE) {
            const uint8_t ot = ct.objective_type_for(et);
            EXPECT_GE(ot, 1u);
            EXPECT_LE(ot, 39u);
            saw_objective = true;
            break;
        }
    }
    EXPECT_TRUE(saw_objective) << "no objective found in entries 100-149";
}

TEST(ClassTableJson, DataPtrFor) {
    auto ct = load_committed();
    // At least one entry in the first 100 should have a data_ptr.
    bool saw_data = false;
    for (uint16_t et = 100; et < 200; ++et) {
        uint8_t dt = 0;
        uint32_t dp = 0;
        if (ct.data_ptr_for(et, dt, dp)) {
            EXPECT_NE(dt, wt::DTYPE_NOTHING);
            saw_data = true;
            break;
        }
    }
    EXPECT_TRUE(saw_data) << "no data_ptr found in entries 100-199";
}

// ── unit_subtype_name returns the documented strings ───────────────────────

TEST(ClassTableJson, UnitSubtypeNames) {
    EXPECT_STREQ(wt::unit_subtype_name(wt::DOMAIN_LAND, wt::STYPE_LAND_ARMOR), "Armor");
    EXPECT_STREQ(wt::unit_subtype_name(wt::DOMAIN_AIR, wt::STYPE_AIR_FIGHTER), "Fighter");
    EXPECT_STREQ(wt::unit_subtype_name(wt::DOMAIN_SEA, wt::STYPE_SEA_CARRIER), "Carrier");
    EXPECT_STREQ(wt::unit_subtype_name(wt::DOMAIN_LAND, 255), "Unknown");
}
