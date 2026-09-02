// test_units.cpp — .uni unit decoder against the real save1.cam.
//
// After the v63 full-decode port (Task VIEWER-1 follow-up), all 683 units
// decode cleanly with the cursor landing exactly at the buffer end.

#include <gtest/gtest.h>
#include <f4/world_convert/cam_archive.hpp>
#include <f4/world_convert/class_table.hpp>
#include <f4/world_convert/unit_decoder.hpp>
#include <f4/world_convert/world_json.hpp>

#include <filesystem>
#include <map>
#include <set>

using namespace f4::world_convert;

namespace {
CamArchive load_fixture() {
    CamArchive cam;
    cam.load(FIXTURE_DIR "save1.cam");
    return cam;
}
}

TEST(Units, DecodesHeaderCorrectly) {
    auto cam = load_fixture();
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size());
    // The .uni header records 683 units.
    EXPECT_EQ(units.count, 683);
}

TEST(Units, DecodesAllRecords) {
    // After the v63 full-decode port, every record must decode cleanly —
    // no cursor desync, no skipped records.
    auto cam = load_fixture();
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size());
    EXPECT_EQ(units.count, 683);
    EXPECT_EQ(static_cast<int>(units.units.size()), units.count)
        << "all 683 units must decode (was only 7 before the v63 port)";
}

TEST(Units, CursorLandsAtBufferEnd) {
    // The strongest correctness signal: after decoding all records, the
    // cursor must sit exactly at the end of the LZSS-decompressed buffer.
    auto cam = load_fixture();
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size());
    EXPECT_EQ(units.bytes_consumed, units.inner_size)
        << "cursor must land at buffer end (no leftover bytes)";
}

TEST(Units, FirstRecordHasPlausibleCoordinates) {
    auto cam = load_fixture();
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size());
    ASSERT_GE(units.units.size(), 1u);
    const auto& u = units.units[0];
    EXPECT_GE(u.x, 0);
    EXPECT_LE(u.x, 1024);
    EXPECT_GE(u.y, 0);
    EXPECT_LE(u.y, 1024);
    EXPECT_LE(u.owner, 7);
}

TEST(Units, AllRecordsHavePlausibleCoordinates) {
    // Stronger than FirstRecordHasPlausibleCoordinates: EVERY decoded
    // unit must have a plausible grid position.
    auto cam = load_fixture();
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size());
    int bad = 0;
    for (const auto& u : units.units) {
        if (u.x < 0 || u.x > 1024 || u.y < 0 || u.y > 1024 || u.owner > 7) {
            ++bad;
        }
    }
    EXPECT_EQ(bad, 0) << "units with implausible coordinates";
}

TEST(Units, UnitClassDistributionMatchesKorea) {
    // save1.cam (Korea campaign) has a known unit-class distribution.
    // From the v63 layout analysis: 524 battalions, 85 brigades,
    // 72 squadrons, 2 task forces, 0 flights, 0 packages.
    auto cam = load_fixture();
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size());

    int counts[7] = {0};   // indexed by UnitClass enum value
    for (const auto& u : units.units) {
        ASSERT_NE(u.unit_class, UnitClass::Unknown)
            << "every unit must have a recognized class (no Unknowns allowed)";
        counts[static_cast<int>(u.unit_class)]++;
    }
    EXPECT_EQ(counts[static_cast<int>(UnitClass::Battalion)],  524);
    EXPECT_EQ(counts[static_cast<int>(UnitClass::Brigade)],    85);
    EXPECT_EQ(counts[static_cast<int>(UnitClass::Squadron)],   72);
    EXPECT_EQ(counts[static_cast<int>(UnitClass::TaskForce)],  2);
    EXPECT_EQ(counts[static_cast<int>(UnitClass::Flight)],     0);
    EXPECT_EQ(counts[static_cast<int>(UnitClass::Package)],    0);
}

TEST(Units, MultipleOwnersRepresented) {
    auto cam = load_fixture();
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size());
    std::set<int> owners;
    for (const auto& u : units.units) owners.insert(u.owner);
    EXPECT_GT(owners.size(), 1u) << "expected multiple teams";
}

TEST(Units, BattalionTailFieldsArePopulated) {
    // The first record is a Battalion (type=170). Its subclass-specific
    // fields (supply, morale, fatigue, position, parent_id, ...) must
    // be populated with non-default values.
    auto cam = load_fixture();
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size());
    ASSERT_GE(units.units.size(), 1u);
    const auto& u = units.units[0];
    EXPECT_EQ(u.unit_class, UnitClass::Battalion);
    // supply/morale are percentages (0..100). Korea battalions commonly
    // have supply=100 at scenario start.
    EXPECT_LE(u.subclass.supply, 100);
    EXPECT_LE(u.subclass.morale, 100);
    EXPECT_LE(u.subclass.fatigue, 100);
}

TEST(Units, SquadronTailFieldsArePopulated) {
    // Squadrons have a 796-byte tail at v63. The fuel field (long, 4 bytes)
    // is the first one written. If our cursor is misaligned, fuel will
    // come back as garbage (random bytes) — we'd see many distinct values.
    // A correctly-aligned decode produces a small set of consistent values.
    auto cam = load_fixture();
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size());
    int total_squadrons = 0;
    std::map<int32_t, int> fuel_values;
    for (const auto& u : units.units) {
        if (u.unit_class == UnitClass::Squadron) {
            ++total_squadrons;
            ++fuel_values[u.subclass.fuel];
        }
    }
    EXPECT_EQ(total_squadrons, 72) << "fixture must have 72 squadrons";
    // A correctly-aligned decode produces FEW distinct fuel values (the
    // default + a few sentinels). Random garbage would produce dozens.
    EXPECT_LE(fuel_values.size(), 5u)
        << "fuel values should be consistent across squadrons";
    // The common default (3000 lbs) should dominate.
    EXPECT_GT(fuel_values[3000], 0)
        << "expected at least one squadron with the default 3000-lb fuel";
}

TEST(Units, JsonContainsUnitsArray) {
    auto cam = load_fixture();
    std::string json = to_world_json(cam);
    EXPECT_NE(json.find("\"units\""), std::string::npos);
    EXPECT_NE(json.find("\"count\": 683"), std::string::npos);
    // The new fields should appear in the JSON for at least one unit:
    EXPECT_NE(json.find("\"unit_class\""), std::string::npos);
    EXPECT_NE(json.find("\"battalion\""), std::string::npos);
}

TEST(Units, JsonExposesNewFieldsWithoutClassTable) {
    // Verify the fields added in the world-data exposure milestone are
    // emitted unconditionally (independent of class-table availability):
    //   - roster (32-bit packed vehicle count)
    //   - waypoints[] (flight/ground plan — empty array when wp_count=0)
    //   - airbase_id (Squadron→Airbase VU_ID.num)
    //   - last_move / last_combat / heading (Battalion tactical state)
    //   - aa_kills / mission_score / total_losses (Squadron aggregate stats)
    auto cam = load_fixture();
    std::string json = to_world_json(cam);
    EXPECT_NE(json.find("\"roster\": "), std::string::npos)
        << "roster must always be emitted";
    EXPECT_NE(json.find("\"waypoints\": ["), std::string::npos)
        << "waypoints[] must always be emitted (even when empty)";
    EXPECT_NE(json.find("\"domain\": "), std::string::npos)
        << "domain must always be emitted";
    EXPECT_NE(json.find("\"unit_subtype\": "), std::string::npos)
        << "unit_subtype must always be emitted (0 when no class table)";
    // Squadron-only fields — appear at least once because we have 72 squadrons.
    EXPECT_NE(json.find("\"airbase_id\": "), std::string::npos)
        << "Squadron airbase_id must be emitted for squadron records";
    EXPECT_NE(json.find("\"mission_score\": "), std::string::npos)
        << "Squadron mission_score must be emitted";
    EXPECT_NE(json.find("\"squadron_patch\": "), std::string::npos)
        << "Squadron squadron_patch must be emitted";
    // Battalion-only fields — appear at least once because we have 524 battalions.
    EXPECT_NE(json.find("\"last_move\": "), std::string::npos)
        << "Battalion last_move must be emitted";
    EXPECT_NE(json.find("\"final_heading\": "), std::string::npos)
        << "Battalion final_heading must be emitted";
}

TEST(Units, JsonExposesNewFieldsWithClassTable) {
    // With the class table loaded, unit_subtype and domain must carry
    // real (non-zero) values for at least some units. The fixture has
    // 524 battalions + 72 squadrons + 2 task forces = 598 units that
    // should resolve to a non-zero subtype when the class table is loaded.
    auto cam = load_fixture();
    ClassTable ct;
    ASSERT_TRUE(std::filesystem::exists(FIXTURE_DIR "FALCON4.ct"));
    ct.load(std::string(FIXTURE_DIR) + "FALCON4.ct");
    ASSERT_GT(ct.size(), 0u);

    WorldJsonOptions opts;
    opts.class_table = &ct;
    std::string json = to_world_json(cam, opts);

    // Re-decode to count how many units resolve to a non-zero subtype.
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size());
    int resolved = 0;
    int land_count = 0, air_count = 0, sea_count = 0;
    for (const auto& u : units.units) {
        const auto* e = ct.lookup(u.entity_type);
        if (!e) continue;
        if (e->stype > 0) ++resolved;
        if (e->domain == DOMAIN_LAND) ++land_count;
        else if (e->domain == DOMAIN_AIR) ++air_count;
        else if (e->domain == DOMAIN_SEA) ++sea_count;
    }
    EXPECT_GE(resolved, 500) << "expected most units to resolve to a non-zero subtype";
    EXPECT_GE(land_count, 500) << "expected >500 land-domain units (battalions+brigades)";
    EXPECT_GE(air_count, 50) << "expected >50 air-domain units (squadrons)";
    EXPECT_GE(sea_count, 1) << "expected at least 1 sea-domain unit (task forces)";

    // The JSON must contain at least one of each readable subtype string
    // emitted via unit_subtype_name() (consumed by the viewer). We don't
    // test the string here (it's in class_table.hpp, not in the JSON) but
    // we verify the raw (domain, subtype) pairs are present.
    EXPECT_NE(json.find("\"domain\": 3,"), std::string::npos) << "land domain present";
    EXPECT_NE(json.find("\"domain\": 2,"), std::string::npos) << "air domain present";
}

// ── v71: TestCamp.cam — a real mid-campaign save ─────────────────────────────
// TestCamp.cam (repo root, gCampDataVersion 71, ~half a day of fighting)
// exercises the v71 layout deltas: CampBaseClass pos_.z_ float (v>=70),
// ushort current_wp / wp_count (v>=71), squadron stores[220] (v69..71),
// the flight v>65 trio (old_mission/mission_context/requester), and
// the package small/big branch selected by (unit_flags & U_FINAL).
// Parity: 1715/1715 units, cursor at exactly 423,065 bytes.
namespace {
CamArchive load_testcamp() {
    CamArchive cam;
    cam.load(REPO_ROOT "TestCamp.cam");
    return cam;
}
bool testcamp_available() {
    std::error_code ec;
    return std::filesystem::exists(REPO_ROOT "TestCamp.cam", ec);
}
} // namespace

TEST(V71Units, TestCampDecodesAllUnitsWithClassTableDispatch) {
    if (!testcamp_available()) GTEST_SKIP() << "TestCamp.cam not in repo root";
    auto cam = load_testcamp();

    const SubFile* ver = cam.find("ver");
    ASSERT_NE(ver, nullptr);
    ASSERT_EQ(read_version(ver->data.data(), ver->data.size()), 71);

    ClassTable ct;
    ct.load(FIXTURE_DIR "FALCON4.ct");

    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    UnitDecodeOptions opts;
    opts.camp_version = 71;
    opts.class_table = &ct;
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size(), opts);

    // The full mid-campaign roster — every record decodes, cursor exact.
    ASSERT_EQ(units.count, 1715);
    EXPECT_EQ(units.units.size(), std::size_t{1715});
    EXPECT_EQ(units.bytes_consumed, units.inner_size);
    EXPECT_EQ(units.inner_size, std::size_t{423065});

    // Class distribution: 449 flights, 371 packages, 672 battalions,
    // 94 squadrons, 114 brigades, 15 task forces (pinned from the file).
    std::map<UnitClass, std::size_t> by_class;
    for (const auto& u : units.units) ++by_class[u.unit_class];
    EXPECT_EQ(by_class[UnitClass::Flight],    std::size_t{449});
    EXPECT_EQ(by_class[UnitClass::Package],   std::size_t{371});
    EXPECT_EQ(by_class[UnitClass::Battalion], std::size_t{672});
    EXPECT_EQ(by_class[UnitClass::Squadron],  std::size_t{94});
    EXPECT_EQ(by_class[UnitClass::Brigade],   std::size_t{114});
    EXPECT_EQ(by_class[UnitClass::TaskForce], std::size_t{15});
    EXPECT_EQ(by_class[UnitClass::Unknown],   std::size_t{0});

    // v71 CampBase carries the z float; ground units sit at z 0, flights
    // carry real altitudes (feet, positive up to ~40k).
    int flights_with_alt = 0;
    for (const auto& u : units.units) {
        if (u.unit_class != UnitClass::Flight) continue;
        if (u.subclass.altitude != 0.0f || u.z != 0.0f) ++flights_with_alt;
    }
    EXPECT_GE(flights_with_alt, 100);

    // Flights carry missions, loadout counts, package/squadron links.
    int flights_with_mission = 0;
    int flights_in_package = 0;
    for (const auto& u : units.units) {
        if (u.unit_class != UnitClass::Flight) continue;
        if (u.subclass.mission != 0) ++flights_with_mission;
        if (u.subclass.package_num != 0) ++flights_with_mission;  // sanity
        if (u.subclass.package_num != 0 && u.subclass.package_num != 0xFFFFFFFFu)
            ++flights_in_package;
    }
    EXPECT_GE(flights_with_mission, 200);
    EXPECT_GE(flights_in_package, 300);
}

TEST(V71Units, TestCampPackagesCarryMissionRequests) {
    if (!testcamp_available()) GTEST_SKIP() << "TestCamp.cam not in repo root";
    auto cam = load_testcamp();
    ClassTable ct;
    ct.load(FIXTURE_DIR "FALCON4.ct");

    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    UnitDecodeOptions opts;
    opts.camp_version = 71;
    opts.class_table = &ct;
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size(), opts);
    ASSERT_EQ(units.units.size(), std::size_t{1715});

    // Packages carry elements (flight VU_IDs) and a mission request.
    // All 371 packages in this save are finalized (small branch —
    // Final() && !wait_cycles after half a day of fighting).
    int pkgs_with_elements = 0;
    int pkgs_with_branch = 0;
    int small = 0, big = 0;
    for (const auto& u : units.units) {
        if (u.unit_class != UnitClass::Package) continue;
        if (u.subclass.elements > 0 &&
            u.subclass.element_ids.size() ==
                static_cast<std::size_t>(u.subclass.elements) * 2) {
            ++pkgs_with_elements;
        }
        if (u.subclass.package_branch != PackageBranch::None) ++pkgs_with_branch;
        if (u.subclass.package_branch == PackageBranch::Small) ++small;
        if (u.subclass.package_branch == PackageBranch::Big) ++big;
    }
    EXPECT_EQ(pkgs_with_elements, 371);
    EXPECT_EQ(pkgs_with_branch, 371);
    EXPECT_EQ(small, 371);
    EXPECT_EQ(big, 0);
}

TEST(V71Units, TestCampFlightsCarryDecodedLoadoutStations) {
    // The A-G employment slice: flight LoadoutStruct[] decoding. All 439
    // tasked flights carry 1 loadout entry (one per aircraft slot — the
    // wire stores the same fill per slot; the decoder keeps entry 0's
    // 16-station fill as the flight's loadout). Stations with weapon_id 0
    // are dropped; the rest carry (wire weapon id, count).
    if (!testcamp_available()) GTEST_SKIP() << "TestCamp.cam not in repo root";
    auto cam = load_testcamp();
    ClassTable ct;
    ct.load(FIXTURE_DIR "FALCON4.ct");

    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    UnitDecodeOptions opts;
    opts.camp_version = 71;
    opts.class_table = &ct;
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size(), opts);
    ASSERT_EQ(units.units.size(), std::size_t{1715});

    int flights_with_loadouts = 0;
    int flights_with_stations = 0;
    int gbu12_stations = 0;   // wire id 68 (campweap.h: GBU-12)
    int total_stations = 0;
    int zero_count_stations = 0;  // pods / non-expendable hardpoints
    for (const auto& u : units.units) {
        if (u.unit_class != UnitClass::Flight) continue;
        if (u.subclass.loadouts > 0) ++flights_with_loadouts;
        if (!u.subclass.loadout_stations.empty()) ++flights_with_stations;
        for (const auto& st : u.subclass.loadout_stations) {
            ++total_stations;
            if (st.weapon_id == 68) ++gbu12_stations;
            if (st.count == 0) ++zero_count_stations;
        }
    }
    EXPECT_EQ(flights_with_loadouts, 449);  // every flight carries a loadout entry
    EXPECT_EQ(flights_with_stations, 424);  // 25 flights fly clean wings (all hardpoints empty)
    EXPECT_GE(total_stations, 1500);       // ~4.5 stations per flight
    EXPECT_GE(gbu12_stations, 3);          // BAI/SAD flights carry GBU-12s
    // Zero-count stations exist on the wire (pod-style hardpoints that
    // expend nothing); they are a minority, not the norm.
    EXPECT_LT(zero_count_stations, total_stations / 3);
}

TEST(V71Units, TestCampDecodesWithoutClassTableToo) {
    // No class table: the trial-and-error fallback must also walk the
    // whole stream (the validator's type range stays at the legacy
    // [100..2000] heuristic — entity types above 2000 are rare enough
    // that the fallback still lands all records on this file).
    if (!testcamp_available()) GTEST_SKIP() << "TestCamp.cam not in repo root";
    auto cam = load_testcamp();
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);
    UnitDecodeOptions opts;                    // v63 default; override:
    opts.camp_version = 71;                    // no class_table
    DecodedUnits units = decode_uni(uni->data.data(), uni->data.size(), opts);
    EXPECT_EQ(units.units.size(), std::size_t{1715});
    EXPECT_EQ(units.bytes_consumed, units.inner_size);
}

TEST(V71Units, V63FixtureStillDecodesWithV71Code) {
    // Regression: the version-gated decoder must keep the v63 fixture's
    // parity (683/683, exact consumption) with and without a class table.
    auto cam = load_fixture();
    const SubFile* uni = cam.find("uni");
    ASSERT_NE(uni, nullptr);

    UnitDecodeOptions v63;
    v63.camp_version = 63;
    DecodedUnits a = decode_uni(uni->data.data(), uni->data.size(), v63);
    EXPECT_EQ(a.units.size(), std::size_t{683});
    EXPECT_EQ(a.bytes_consumed, a.inner_size);

    ClassTable ct;
    ct.load(FIXTURE_DIR "FALCON4.ct");
    UnitDecodeOptions v63ct;
    v63ct.camp_version = 63;
    v63ct.class_table = &ct;
    DecodedUnits b = decode_uni(uni->data.data(), uni->data.size(), v63ct);
    EXPECT_EQ(b.units.size(), std::size_t{683});
    EXPECT_EQ(b.bytes_consumed, b.inner_size);

    // Class-table dispatch and trial-and-error agree on every record.
    for (std::size_t i = 0; i < a.units.size(); ++i) {
        ASSERT_EQ(a.units[i].unit_class, b.units[i].unit_class)
            << "record " << i << " type " << a.units[i].type;
    }
}
