// f4-ai/include/f4/ai/target_info.hpp
//
// TargetInfo — the per-target snapshot produced by SensorFusion.
//
// This is the AI's "picture" of one detected entity. One TargetInfo per
// detected entity, refreshed on the skill-dependent update interval.
//
// Field groups:
//   - Identity:       entity_id (matches EntityId::value in f4-entities)
//   - Geometry:       range, azimuth, elevation, ATA (recomputed each update)
//   - EWMA rates:     atadot, rangedot (carry state across updates)
//   - Threat:         threat_score, combat_class, is_missile, is_hostile
//   - Detection:      detected_by_{radar,rwr,visual,gci} — any one = can_see
//   - Cached state:   position, velocity (for downstream modules)
//
// FreeFalcon reference: this struct consolidates the fields of `FalconEntity`
// that `sfusion.cpp` and `targeting.cpp` actually read. The original code
// reached into the entity's member variables directly; we snapshot them into
// a plain struct so the AI never touches the EntityWorld during tactic
// evaluation — only during SensorFusion::update().

#pragma once

#include <cstdint>

#include <f4/geo/position.hpp>

namespace f4::ai {

struct TargetInfo {
    // --- Identity ---
    std::uint64_t entity_id{0};

    // --- Geometry (recomputed each update) ---
    double range_ft{0.0};        // slant range to target
    double range_nm{0.0};        // nautical miles (= range_ft / 6076.11548)
    double azimuth_rad{0.0};     // bearing from ownship nose, CW, [0, 2*pi)
    double elevation_rad{0.0};   // elevation angle to target (rad)

    // Aspect Tail Angle (ATA): the angle off the target's nose, measured
    // from the target's perspective.
    //   0     = target's nose is pointed directly at us (nose-on, most dangerous)
    //   pi    = target's tail is toward us (target is running away)
    // FreeFalcon uses this in the threat-scoring rule: if ATA > pi/2,
    // halve the score (target is pointed away = less threatening).
    double ata_rad{0.0};

    // ATA from ownship perspective: angle between ownship's velocity and the
    // ownship->target vector. 0 = we're pointed at the target.
    double ata_from_rad{0.0};

    // --- EWMA-smoothed rates (carry state across updates) ---
    // FreeFalcon blends with 0.85 (old) / 0.15 (new) per update. The rate
    // is per-second (delta / update_interval), so it's frame-rate independent.
    double atadot{0.0};          // d(ATA)/dt, EWMA smoothed (rad/s)
    double rangedot{0.0};        // d(range)/dt, EWMA smoothed (ft/s)
                                 // positive = closing, negative = opening

    // --- Threat assessment ---
    double threat_score{0.0};
    int    combat_class{0};      // 0=stationary, 1=possible, 2=armed,
                                 // 3=maneuvering, 4=fighter
    bool   is_missile{false};
    bool   is_hostile{false};

    // --- Detection sources (any one enables can_see) ---
    // GCI (Ground Controlled Intercept) sees everything within the theater
    // by definition — it's the AWACS/GCI radar net. Radar/RWR/Visual are
    // range-gated to the ownship's sensor footprint.
    bool   detected_by_radar{false};
    bool   detected_by_rwr{false};
    bool   detected_by_visual{false};
    bool   detected_by_gci{false};

    // --- Cached target state (snapshot at last refresh) ---
    f4::geo::WorldPosition position{};
    f4::geo::WorldPosition velocity{};  // world-frame velocity (ft/s)

    /// Seconds since the fusion rebuilt this entry (0 = fresh). The
    /// fusion refreshes at the SKILL interval (1-10 s), not per tick —
    /// a track file, not a live feed. Consumers that need NOW-geometry
    /// at tick rate (the gun predictor) dead-reckon:
    ///     now_position = position + velocity * age_s.
    /// Missile envelopes absorb staleness by design; age 0 (the default
    /// every hand-built TargetInfo carries) means the snapshot IS now.
    double age_s{0.0};
};

} // namespace f4::ai
