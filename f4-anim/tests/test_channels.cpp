// f4-anim/tests/test_channels.cpp
//
// Channel vocabulary tests: enum/name table sync, round-trip parsing,
// AnimValues presets, and the Process_DOFRot port against the exact
// FreeFalcon flag semantics.

#include <f4/anim/channels.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <string>

using namespace f4::anim;

// ── Name table sync ───────────────────────────────────────────────────────

TEST(AnimChannels, EveryIdHasADistinctName) {
    for (uint16_t i = 0; i < kChannelCount; ++i) {
        const char* name = channel_name(static_cast<Channel>(i));
        ASSERT_NE(name, nullptr);
        ASSERT_STRNE(name, "unknown") << "id " << i << " has no name";
        // Distinctness: parse must round-trip to the same id.
        auto parsed = channel_from_name(name);
        ASSERT_TRUE(parsed.has_value())
            << "name '" << name << "' does not parse back";
        ASSERT_EQ(static_cast<uint16_t>(*parsed), i)
            << "name '" << name << "' collides with another channel";
    }
}

TEST(AnimChannels, CountNameIsUnknown) {
    EXPECT_STREQ(channel_name(Channel::Count), "unknown");
    EXPECT_FALSE(channel_from_name("no_such_channel").has_value());
    EXPECT_FALSE(channel_from_name("").has_value());
}

TEST(AnimChannels, KnownNamesParse) {
    EXPECT_EQ(channel_from_name("gear_pos"), Channel::gear_pos);
    EXPECT_EQ(channel_from_name("gear_leg_pos.0"), Channel::gear_leg_pos_0);
    EXPECT_EQ(channel_from_name("gear_leg_pos.7"), Channel::gear_leg_pos_7);
    EXPECT_EQ(channel_from_name("sw.gear_leg.3"), Channel::sw_gear_leg_3);
    EXPECT_EQ(channel_from_name("sw.ab"), Channel::sw_ab);
    EXPECT_EQ(channel_from_name("stab.l"), Channel::stab_l);
    EXPECT_EQ(channel_from_name("rotor.main"), Channel::rotor_main);
    EXPECT_EQ(channel_from_name("light.strobe"), Channel::light_strobe);
}

TEST(AnimChannels, VocabularyCoversThePlan) {
    // The channel ids the rest of the stack hard-references must sit at
    // their documented offsets (the gear block is indexed arithmetically
    // by station number — its layout is load-bearing).
    EXPECT_EQ(static_cast<uint16_t>(Channel::gear_leg_pos_1),
              static_cast<uint16_t>(Channel::gear_leg_pos_0) + 1);
    EXPECT_EQ(static_cast<uint16_t>(Channel::sw_gear_leg_7),
              static_cast<uint16_t>(Channel::sw_gear_leg_0) + 7);
    EXPECT_EQ(static_cast<uint16_t>(Channel::sw_gear_door_5),
              static_cast<uint16_t>(Channel::sw_gear_door_0) + 5);
    EXPECT_EQ(static_cast<uint16_t>(Channel::sw_gear_hole_2),
              static_cast<uint16_t>(Channel::sw_gear_hole_0) + 2);
    EXPECT_EQ(static_cast<uint16_t>(Channel::sw_gear_broken_4),
              static_cast<uint16_t>(Channel::sw_gear_broken_0) + 4);
    EXPECT_GT(kChannelCount, 100u);
}

// ── AnimValues ────────────────────────────────────────────────────────────

TEST(AnimValues, DefaultsToZero) {
    AnimValues v;
    for (uint16_t i = 0; i < kChannelCount; ++i) {
        ASSERT_EQ(v[i], 0.0f);
    }
}

TEST(AnimValues, ParkedDefaultsShowGear) {
    AnimValues v;
    v.set_parked_defaults();
    EXPECT_EQ(v[Channel::gear_pos], 1.0f);
    for (uint16_t s = 0; s < kGearStations; ++s) {
        EXPECT_EQ(v[static_cast<Channel>(
                      static_cast<uint16_t>(Channel::sw_gear_leg_0) + s)],
                  1.0f);
        EXPECT_EQ(v[static_cast<Channel>(
                      static_cast<uint16_t>(Channel::sw_gear_door_0) + s)],
                  1.0f);
        EXPECT_EQ(v[static_cast<Channel>(
                      static_cast<uint16_t>(Channel::sw_gear_hole_0) + s)],
                  1.0f);
    }
    // Effects stay off on the ground.
    EXPECT_EQ(v[Channel::sw_ab], 0.0f);
    EXPECT_EQ(v[Channel::effect_vapor], 0.0f);
}

// ── process_dof_value (FreeFalcon Process_DOFRot semantics) ──────────────

TEST(ProcessDofValue, PlainValuePassesThroughWithMultiplier) {
    EXPECT_FLOAT_EQ(process_dof_value(0.5f, 0, 0.f, 0.f, 1.f), 0.5f);
    EXPECT_FLOAT_EQ(process_dof_value(0.5f, 0, 0.f, 0.f, 2.f), 1.0f);
}

TEST(ProcessDofValue, NegateFlag) {
    EXPECT_FLOAT_EQ(
        process_dof_value(0.25f, kXdofNegate, 0.f, 0.f, 1.f), -0.25f);
}

TEST(ProcessDofValue, MinmaxClamps) {
    EXPECT_FLOAT_EQ(
        process_dof_value(-1.0f, kXdofMinmax, 0.f, 1.f, 1.f), 0.0f);
    EXPECT_FLOAT_EQ(
        process_dof_value(7.0f, kXdofMinmax, 0.f, 1.f, 1.f), 1.0f);
    EXPECT_FLOAT_EQ(
        process_dof_value(0.5f, kXdofMinmax, 0.f, 1.f, 1.f), 0.5f);
}

TEST(ProcessDofValue, SubrangeRescales) {
    // [min,max] = [10, 20]: 10 → 0, 15 → 0.5, 20 → 1.
    EXPECT_FLOAT_EQ(
        process_dof_value(10.f, kXdofSubrange, 10.f, 20.f, 1.f), 0.0f);
    EXPECT_FLOAT_EQ(
        process_dof_value(15.f, kXdofSubrange, 10.f, 20.f, 1.f), 0.5f);
    EXPECT_FLOAT_EQ(
        process_dof_value(20.f, kXdofSubrange, 10.f, 20.f, 1.f), 1.0f);
}

TEST(ProcessDofValue, SubrangeWithIsDofConvertsDegreesToRadians) {
    // After the rescale to [0,1], the value is interpreted as degrees
    // and converted to radians: 0.5 * (180/π in deg→rad) = 0.5 rad.
    const int32_t flags = kXdofSubrange | kXdofIsDof;
    EXPECT_NEAR(process_dof_value(0.5f, flags, 0.f, 1.f, 1.f),
                0.5f * 0.017453293f, 1e-9f);
}

TEST(ProcessDofValue, FlagsCompose) {
    // NEGATE then MINMAX then SUBRANGE then multiplier — the exact
    // FreeFalcon order (bspnodes.cpp Process_DOFRot).
    const int32_t flags = kXdofNegate | kXdofMinmax;
    // 0.25 negated → -0.25, clamp [0,1] → 0.0, * 4 → 0.0.
    EXPECT_FLOAT_EQ(process_dof_value(0.25f, flags, 0.f, 1.f, 4.f), 0.0f);
    // -0.25 negated → 0.25, clamp → 0.25, * 4 → 1.0.
    EXPECT_FLOAT_EQ(process_dof_value(-0.25f, flags, 0.f, 1.f, 4.f), 1.0f);
}
