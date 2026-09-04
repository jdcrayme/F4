// f4-ai/include/f4/ai/air_picture.hpp
//
// AirPicture — the host-built, shared air-picture snapshot (PERF-1,
// PERFORMANCE_PLAN.md §3).
//
// SensorFusion's world-query rebuild walks every transform-bearing entity
// (~4,400 in a populated campaign save) per rebuild. At campaign scale
// with C6's beam-fight rule — every brain force-refreshes its picture
// every tick while a hostile missile is visible — that walk runs 96
// brains x 60 Hz x 4,400 candidates: the shared picture re-derived from
// the database per-brain, per-tick (measured: the merge-phase collapse,
// 4 ms -> 88 ms per tick with the roster flat; see the plan's §1).
//
// The reference never does this: FreeFalcon's campaign loop iterates its
// VU entity collections ONCE per update and digi targeting reads shared
// iteration state. This type is that shape for our stack: the HOST walks
// the world once per tick, builds the snapshot (contacts in
// entity-index order — exactly the candidate set and order
// with_component<TransformComponent>() produced, clutter-skipped by the
// same TransformComponent::is_ground_clutter() rule), and pushes one
// non-owning pointer to every brain. SensorFusion rebuilds from it with
// byte-identical output; the world-query path stays first-class for
// hosts and tests that push nothing.
//
// The type is deliberately dependency-light: f4::geo positions, a team
// interning table, plain values — no EntityWorld, no components, no
// f4-sensors. f4-ai stays engine-agnostic; the picture is DATA.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <f4/geo/position.hpp>

namespace f4::ai {

/// One non-clutter entity in the shared air picture. Field-for-field the
/// world state SensorFusion's per-candidate loop read: identity, ENU
/// position/velocity (feet, feet/s — the transform's frame), team
/// membership (interned index into AirPicture::teams), and the missile
/// role bit (the ROLE tag == "missile" test the rebuild ran per
/// candidate).
struct AirPictureContact {
    std::uint64_t entity_id{0};
    f4::geo::WorldPosition position{};
    f4::geo::WorldPosition velocity{};  // ENU ft/s (x=east, y=north, z=up)
    std::int16_t team{-1};   // index into AirPicture::teams; -1 = no team tag
    bool is_missile{false};  // ROLE tag == "missile"
};

/// The per-tick shared snapshot. Contacts are in entity-index order —
/// the order the component-type-index bucket walk yields — which is the
/// order the per-brain rebuild iterates candidates in. Order matters:
/// TargetInfo tie-breaks (threat queries keep the FIRST max) are
/// iteration-order-sensitive, so the snapshot preserves it exactly.
///
/// `teams` interns distinct team strings in first-seen order. The
/// consumer compares by STRING (own-relative hostility, the legacy
/// "red" rule); interning only exists so each contact carries an index
/// instead of a string copy. The table is rebuilt per tick by the host
/// (team rosters are a handful of entries — linear-scan interning).
struct AirPicture {
    std::vector<AirPictureContact> contacts;
    std::vector<std::string> teams;

    /// Resolve a contact's team index to its string. nullptr when the
    /// contact carries no team (or the index is out of range —
    /// defensive; the host only emits valid indices).
    [[nodiscard]] const std::string* team_name(
        std::int16_t index) const noexcept {
        if (index < 0 ||
            static_cast<std::size_t>(index) >= teams.size()) {
            return nullptr;
        }
        return &teams[static_cast<std::size_t>(index)];
    }
};

} // namespace f4::ai
